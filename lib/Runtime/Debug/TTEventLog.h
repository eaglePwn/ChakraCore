//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

////
//Define compact macros for use in the JSRT API's
#if ENABLE_TTD
#define PERFORM_JSRT_TTD_RECORD_ACTION_CHECK(CTX) (CTX)->ShouldPerformRecordAction()
#define PERFORM_JSRT_TTD_RECORD_ACTION_SIMPLE(CTX, ACTION_CODE) if(PERFORM_JSRT_TTD_RECORD_ACTION_CHECK(CTX)) { (ACTION_CODE); }

//TODO: find and replace all of the occourences of this in jsrt.cpp
#define PERFORM_JSRT_TTD_RECORD_ACTION_NOT_IMPLEMENTED(CTX) if(PERFORM_JSRT_TTD_RECORD_ACTION_CHECK(CTX)) { AssertMsg(false, "Need to implement support here!!!"); }

#define PERFORM_JSRT_TTD_TAG_ACTION(CTX, VAL_PTR) if(VAL_PTR != nullptr ) { TTD::RuntimeThreadInfo::JsRTTagObject((CTX)->GetThreadContext(), *(VAL_PTR)); }
#else
#define PERFORM_JSRT_TTD_RECORD_ACTION_CHECK(CTX) false
#define PERFORM_JSRT_TTD_RECORD_ACTION(ACTION_CODE)
#define PERFORM_JSRT_TTD_RECORD_ACTION_NOT_IMPLEMENTED(CTX)

#define PERFORM_JSRT_TTD_TAG_ACTION(CTX, VAL)
#endif

////
//Begin the regular TTD code

#if ENABLE_TTD

#define TTD_EVENTLOG_LIST_BLOCK_SIZE 512

namespace TTD
{
    //A class to ensure that even when exceptions are thrown the pop action for the TTD call stack is executed
    class TTDExceptionFramePopper
    {
    private:
        EventLog* m_log;
        Js::JavascriptFunction* m_function;

    public:
        TTDExceptionFramePopper();
        ~TTDExceptionFramePopper();

        void PushInfo(EventLog* log, Js::JavascriptFunction* function);
        void PopInfo();
    };

    //A class to ensure that even when exceptions are thrown we record the time difference info
    class TTDRecordExternalFunctionCallActionPopper
    {
    private:
        EventLog* m_log;
        Js::JavascriptFunction* m_function;
        Js::HiResTimer m_timer;
        double m_startTime;

        ExternalCallEventBeginLogEntry* m_callAction;

    public:
        TTDRecordExternalFunctionCallActionPopper(EventLog* log, Js::JavascriptFunction* function);
        ~TTDRecordExternalFunctionCallActionPopper();

        void NormalReturn(bool checkException, Js::Var returnValue);

        void SetCallAction(ExternalCallEventBeginLogEntry* action);
        double GetStartTime();
    };

    //A class to ensure that even when exceptions are thrown we record the time difference info
    class TTDRecordJsRTFunctionCallActionPopper
    {
    private:
        EventLog* m_log;
        Js::ScriptContext* m_scriptContext;
        Js::HiResTimer m_timer;
        double m_startTime;

        JsRTCallFunctionBeginAction* m_callAction;

    public:
        TTDRecordJsRTFunctionCallActionPopper(EventLog* log, Js::ScriptContext* scriptContext);
        ~TTDRecordJsRTFunctionCallActionPopper();

        void NormalReturn();

        void SetCallAction(JsRTCallFunctionBeginAction* action);
        double GetStartTime();
    };

    //A list class for the events that we accumulate in the event log
    class TTEventList
    {
        struct TTEventListLink
        {
            //The current end of the allocated data in the block
            uint32 CurrPos;

            //The First index that holds data
            uint32 StartPos;

            //The actual block for the data
            EventLogEntry** BlockData;

            //The next block in the list
            TTEventListLink* Next;
            TTEventListLink* Previous;
        };

        //The the data in this
        TTEventListLink* m_headBlock;

        //the allocators we use for this work
        UnlinkableSlabAllocator* m_alloc;

        void AddArrayLink();
        void RemoveArrayLink(TTEventListLink* block);

    public:
        TTEventList(UnlinkableSlabAllocator* alloc);
        void UnloadEventList();

        //Add the entry to the list
        void AddEntry(EventLogEntry* data);

        //Delete the entry from the list (must always be the first link/entry in the list)
        //This also calls unload on the entry
        void DeleteFirstEntry(TTEventListLink* block, EventLogEntry* data);

        //Return true if this is empty
        bool IsEmpty() const;

        //NOT constant time
        uint32 Count() const;

        class Iterator
        {
        private:
            TTEventListLink* m_currLink;
            uint32 m_currIdx;

        public:
            Iterator();
            Iterator(TTEventListLink* head, uint32 pos);

            const EventLogEntry* Current() const;
            EventLogEntry* Current();

            bool IsValid() const;

            void MoveNext();
            void MovePrevious();
        };

        Iterator GetIteratorAtFirst() const;
        Iterator GetIteratorAtLast() const;
    };

    //A struct for tracking time events in a single method
    struct SingleCallCounter
    {
        Js::FunctionBody* Function;

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        LPCWSTR Name; //only added for debugging can get rid of later.
#endif

        uint64 EventTime; //The event time when the function was called
        uint64 FunctionTime; //The function time when the function was called
        uint64 LoopTime; //The current loop taken time for the function

#if ENABLE_TTD_STACK_STMTS
        int32 LastStatementIndex; //The previously executed statement
        uint64 LastStatementLoopTime; //The previously executed statement

        int32 CurrentStatementIndex; //The currently executing statement
        uint64 CurrentStatementLoopTime; //The currently executing statement

        //bytecode range of the current stmt
        uint32 CurrentStatementBytecodeMin;
        uint32 CurrentStatementBytecodeMax;
#endif
    };

    //A class that represents the event log for the program execution
    class EventLog
    {
    private:
        ThreadContext* m_threadContext;

        //Allocator we use for all the events we see
        UnlinkableSlabAllocator m_eventSlabAllocator;

        //Allocator we use for all the property records
        SlabAllocator m_miscSlabAllocator;

        //The root directory that the log info gets stored into
        TTString m_logInfoRootDir;

        //The interval between snapshots we want to use and the history length we want to keep (at least 2)
        uint32 m_snapInterval;
        uint32 m_snapHistoryLength;

        //The global event time variable
        int64 m_eventTimeCtr;

        //A counter (per event dispatch) which holds the current value for the function counter
        uint64 m_runningFunctionTimeCtr;

        //Top-Level callback event time (or -1 if we are not in a callback)
        int64 m_topLevelCallbackEventTime;

        //The tag (from the host) that tells us which callback id this (toplevel) callback is associated with (-1 if not initiated by a callback)
        int64 m_hostCallbackId;

        //The list of all the events and the iterator we use during replay
        TTEventList m_eventList;
        TTEventList::Iterator m_currentReplayEventIterator;

        //Array of call counters (used as stack)
        JsUtil::List<SingleCallCounter, HeapAllocator> m_callStack;

        //The current mode the system is running in (and a stack of mode push/pops that we use to generate it)
        JsUtil::List<TTDMode, HeapAllocator> m_modeStack;
        TTDMode m_currentMode;

        //A list of contexts that are being run in TTD mode (and the associated callback functors) -- We assume the host creates a single context for now 
        Js::ScriptContext* m_ttdContext;

        //The snapshot extractor that this log uses
        SnapshotExtractor m_snapExtractor;

        //The execution time that has elapsed since the last snapshot
        double m_elapsedExecutionTimeSinceSnapshot;

        //If we are inflating a snapshot multiple times we want to re-use the inflated objects when possible so keep this recent info
        int64 m_lastInflateSnapshotTime;
        InflateMap* m_lastInflateMap;

        //Pin set of all property records created during this logging session
        PropertyRecordPinSet* m_propertyRecordPinSet;
        UnorderedArrayList<NSSnapType::SnapPropertyRecord, TTD_ARRAY_LIST_SIZE_DEFAULT> m_propertyRecordList;

        //A list of all *root* scripts that have been loaded during this session
        UnorderedArrayList<NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo, TTD_ARRAY_LIST_SIZE_MID> m_loadedTopLevelScripts;
        UnorderedArrayList<NSSnapValues::TopLevelNewFunctionBodyResolveInfo, TTD_ARRAY_LIST_SIZE_SMALL> m_newFunctionTopLevelScripts;
        UnorderedArrayList<NSSnapValues::TopLevelEvalFunctionBodyResolveInfo, TTD_ARRAY_LIST_SIZE_SMALL> m_evalTopLevelScripts;

#if ENABLE_TTD_DEBUGGING
        //The most recently executed statement before return -- normal return or exception
        //We clear this after executing any following statements so this can be used for:
        // - Step back to uncaught exception
        // - Step to last statement in previous event
        // - Step back *into* if either of the flags are true
        bool m_isReturnFrame;
        bool m_isExceptionFrame;
        SingleCallCounter m_lastFrame;

        //A pending TTDBP we want to set and move to
        TTDebuggerSourceLocation m_pendingTTDBP;

        //The bp we are actively moving to in TT mode
        int64 m_activeBPId;
        TTDebuggerSourceLocation m_activeTTDBP;
#endif

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        TraceLogger m_diagnosticLogger;
#endif

        ////
        //Helper methods

        //get the top call counter from the stack
        const SingleCallCounter& GetTopCallCounter() const;
        SingleCallCounter& GetTopCallCounter();

        //get the caller for the top call counter from the stack (e.g. stack -2)
        const SingleCallCounter& GetTopCallCallerCounter() const;

        //Get the current XTTDEventTime and advance the event time counter
        int64 GetCurrentEventTimeAndAdvance();

        //Advance the time and event position for replay
        void AdvanceTimeAndPositionForReplay();

        //insert the event at the head of the events list
        void InsertEventAtHead(EventLogEntry* evnt);

        //Look at the stack to get the new computed mode
        void UpdateComputedMode();

        //Unload any pinned or otherwise retained objects
        void UnloadRetainedData();

        //A helper for extracting snapshots
        void DoSnapshotExtract_Helper(SnapShot** snap, TTD_LOG_TAG* logTag);

        //Replay a snapshot event -- either just advance the event position or, if running diagnostics, take new snapshot and compare
        void ReplaySnapshotEvent();

    public:
        EventLog(ThreadContext* threadContext, LPCWSTR logDir, uint32 snapInterval, uint32 snapHistoryLength);
        ~EventLog();

        //Initialize the log so that it is ready to perform TTD (record or replay) and set into the correct global mode
        void InitForTTDRecord();
        void InitForTTDReplay();

        //Add/remove script contexts from time travel
        void StartTimeTravelOnScript(Js::ScriptContext* ctx, const HostScriptContextCallbackFunctor& callbackFunctor);
        void StopTimeTravelOnScript(Js::ScriptContext* ctx);

        //reset the bottom (global) mode with the specific value
        void SetGlobalMode(TTDMode m);

        //push a new debugger mode 
        void PushMode(TTDMode m);

        //pop the top debugger mode
        void PopMode(TTDMode m);

        //Set the log into debugging mode (it must already be in replay mode)
        void SetIntoDebuggingMode();

        //Use this to check specifically if we are in record AND this code is being run on behalf of the user application when doing symbol creation
        bool ShouldPerformRecordAction_SymbolCreation() const
        {
            //return true if RecordEnabled and ~ExcludedExecution
            return (this->m_currentMode & TTD::TTDMode::TTDShouldRecordActionMask) == TTD::TTDMode::RecordEnabled;
        }

        //Use this to check specifically if we are in debugging mode AND this code is being run on behalf of the user application when doing symbol creation
        bool ShouldPerformDebugAction_SymbolCreation() const
        {
#if ENABLE_TTD_DEBUGGING
            //return true if DebuggingEnabled and ~ExcludedExecution
            return (this->m_currentMode & TTD::TTDMode::TTDShouldDebugActionMask) == TTD::TTDMode::DebuggingEnabled;

#else
            return false;
#endif
        }

        //Add a property record to our pin set
        void AddPropertyRecord(const Js::PropertyRecord* record);

        //Add top level function load info to our sets
        const NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo* AddScriptLoad(Js::FunctionBody* fb, Js::ModuleID moduleId, DWORD_PTR documentID, LPCWSTR source, uint32 sourceLen, LoadScriptFlag loadFlag);
        const NSSnapValues::TopLevelNewFunctionBodyResolveInfo* AddNewFunction(Js::FunctionBody* fb, Js::ModuleID moduleId, LPCWSTR source, uint32 sourceLen);
        const NSSnapValues::TopLevelEvalFunctionBodyResolveInfo* AddEvalFunction(Js::FunctionBody* fb, Js::ModuleID moduleId, LPCWSTR source, uint32 sourceLen, ulong grfscr, bool registerDocument, BOOL isIndirect, BOOL strictMode);

        void RecordTopLevelCodeAction(uint64 bodyCtrId);
        uint64 ReplayTopLevelCodeAction();

        ////////////////////////////////
        //Logging support

        //Log an event generated by user telemetry
        void RecordTelemetryLogEvent(Js::JavascriptString* infoStringJs, bool doPrint);

        //Replay a user telemetry event
        void ReplayTelemetryLogEvent(Js::JavascriptString* infoStringJs);

        //Log a time that is fetched during date operations
        void RecordDateTimeEvent(double time);

        //Log a time (as a string) that is fetched during date operations
        void RecordDateStringEvent(Js::JavascriptString* stringValue);

        //replay date event (which should be the current event)
        void ReplayDateTimeEvent(double* result);

        //replay date event with a string result (which should be the current event)
        void ReplayDateStringEvent(Js::ScriptContext* ctx, Js::JavascriptString** result);

        //Log a random seed value that is being generated using external entropy
        void RecordExternalEntropyRandomEvent(uint64 seed0, uint64 seed1);

        //Replay a random seed value that is being generated using external entropy
        void ReplayExternalEntropyRandomEvent(uint64* seed0, uint64* seed1);

        //Log property enumeration step
        void RecordPropertyEnumEvent(BOOL returnCode, Js::PropertyId pid, Js::PropertyAttributes attributes, Js::JavascriptString* propertyName);

        //Replay a property enumeration step
        void ReplayPropertyEnumEvent(BOOL* returnCode, int32* newIndex, const Js::DynamicObject* obj, Js::PropertyId* pid, Js::PropertyAttributes* attributes, Js::JavascriptString** propertyName);

        //Log symbol creation
        void RecordSymbolCreationEvent(Js::PropertyId pid);

        //Replay symbol creation
        void ReplaySymbolCreationEvent(Js::PropertyId* pid);

        //Log a value event for return from an external call
        ExternalCallEventBeginLogEntry* RecordExternalCallBeginEvent(Js::JavascriptFunction* func, int32 rootDepth, uint32 argc, Js::Var* argv, double beginTime);
        void RecordExternalCallEndEvent(Js::JavascriptFunction* func, int64 matchingBeginTime, int32 rootNestingDepth, bool hasScriptException, bool hasTerminatingException, double endTime, Js::Var value);

        ExternalCallEventBeginLogEntry* RecordEnqueueTaskBeginEvent(int32 rootDepth, double beginTime);
        void RecordEnqueueTaskEndEvent(int64 matchingBeginTime, int32 rootDepth, double endTime, Js::Var value);

        //replay an external return event (which should be the current event)
        void ReplayExternalCallEvent(Js::JavascriptFunction* function, uint32 argc, Js::Var* argv, Js::Var* result);

        void ReplayEnqueueTaskEvent(Js::ScriptContext* ctx, Js::Var* result);

        //Log a function call
        void PushCallEvent(Js::JavascriptFunction* function, uint32 argc, Js::Var* argv, bool isInFinally);

        //Log a function return in normal case and exception
        void PopCallEvent(Js::JavascriptFunction* function, Js::Var result);
        void PopCallEventException(Js::JavascriptFunction* function, bool isFirstException);

#if ENABLE_TTD_DEBUGGING
        //To update the exception frame & last return frame info and access it in JSRT
        bool HasImmediateReturnFrame() const;
        bool HasImmediateExceptionFrame() const;
        const SingleCallCounter& GetImmediateReturnFrame() const;
        const SingleCallCounter& GetImmediateExceptionFrame() const;
        void ClearReturnFrame();
        void ClearExceptionFrame();
        void SetReturnAndExceptionFramesFromCurrent(bool setReturn, bool setException);

        bool HasPendingTTDBP() const;
        int64 GetPendingTTDBPTargetEventTime() const;
        void GetPendingTTDBPInfo(TTDebuggerSourceLocation& BPLocation) const;
        void ClearPendingTTDBPInfo();
        void SetPendingTTDBPInfo(const TTDebuggerSourceLocation& BPLocation);

        bool HasActiveBP() const;
        UINT GetActiveBPId() const;
        void ClearActiveBP();
        void SetActiveBP(UINT bpId, const TTDebuggerSourceLocation& bpLocation);

        //Process the breakpoint info as we enter a break statement and return true if we actually want to break
        bool ProcessBPInfoPreBreak(Js::FunctionBody* fb);

        //Process the breakpoint info as we resume from a break statement
        void ProcessBPInfoPostBreak(Js::FunctionBody* fb);
#endif

        //Update the loop count information
        void UpdateLoopCountInfo();

#if ENABLE_TTD_STACK_STMTS
        //
        //TODO: This is not great performance wise
        //
        //For debugging we currently brute force track the current/last source statements executed
        void UpdateCurrentStatementInfo(uint bytecodeOffset);

        //Get the current time/position info for the debugger -- all out arguments are optional (nullptr if you don't care)
        void GetTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const;
#endif

#if ENABLE_OBJECT_SOURCE_TRACKING
        void GetTimeAndPositionForDiagnosticObjectTracking(DiagnosticOrigin& originInfo) const;
#endif 

#if ENABLE_TTD_DEBUGGING
        //Get the previous statement time/position for the debugger -- return false if this is the first statement of the event handler
        bool GetPreviousTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const;

        //Get the last (uncaught or just caught) exception time/position for the debugger -- return true if the last return action was an exception and we have not made any additional calls
        bool GetExceptionTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const;

        //Get the last statement in the just executed call time/position for the debugger -- return true if callerPreviousStmtIndex is the same as the stmt index for this (e.g. this is the immediately proceeding call)
        bool GetImmediateReturnTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const;

        //Get the current host callback id
        int64 GetCurrentHostCallbackId() const;

        //Get the current top-level event time 
        int64 GetCurrentTopLevelEventTime() const;

        //Get the time info around a host id creation/cancelation event -- return null if we can't find the event of interest (not in log or we were called directly by host -- host id == -1)
        JsRTCallbackAction* GetEventForHostCallbackId(bool wantRegisterOp, int64 hostIdOfInterest) const;

        //Get the event time corresponding to the k-th top-level event in the log
        int64 GetKthEventTime(uint32 k) const;
#endif

        //Ensure the call stack is clear and counters are zeroed appropriately
        void ResetCallStackForTopLevelCall(int64 topLevelCallbackEventTime, int64 hostCallbackId);

        //Check if we want to take a snapshot
        bool IsTimeForSnapshot() const;

        //After a snapshot we may want to discard old events so do that in here as needed
        void PruneLogLength();

        //Get/Increment the elapsed time since the last snapshot
        void IncrementElapsedSnapshotTime(double addtlTime);

        ////////////////////////////////
        //Snapshot and replay support

        //Sometimes we need to abort replay and immediately return to the top-level host (debugger) so it can decide what to do next
        //    (1) If we are trying to replay something and we are at the end of the log then we need to terminate
        //    (2) If we are at a breakpoint and we want to step back (in some form) then we need to terminate
        void AbortReplayReturnToHost();

        //Do the snapshot extraction 
        void DoSnapshotExtract();

        //Take a ready-to-run snapshot for the event if needed 
        void DoRtrSnapIfNeeded();

        //Find the event time that has the snapshot we want to inflate from in order to replay to the requested target time
        //Return -1 if no such snapshot is available and set newCtxsNeed true if we want to inflate with "fresh" script contexts
        int64 FindSnapTimeForEventTime(int64 targetTime, bool* newCtxsNeeded);

        //If we decide to update with fresh contexts before the inflate then this will update the inflate map info in the log
        void UpdateInflateMapForFreshScriptContexts();

        //Do the inflation of the snapshot that is at the given event time
        void DoSnapshotInflate(int64 etime);

        //For replay the from the current event (should either be a top-level call/code-load action or a snapshot)
        void ReplaySingleEntry();

        //Run until the given top-level call event time
        void ReplayToTime(int64 eventTime);

        //For debugging replay the full trace from the current event
        void ReplayFullTrace();

        ////////////////////////////////
        //Host API record & replay support

        //Record conversions and symbol creation
        void RecordJsRTVarToObjectConversion(Js::ScriptContext* ctx, Js::Var var);
        void RecordJsRTCreateSymbol(Js::ScriptContext* ctx, Js::Var var);

        //Record object allocate operations
        void RecordJsRTAllocateBasicObject(Js::ScriptContext* ctx, bool isRegularObject);
        void RecordJsRTAllocateBasicClearArray(Js::ScriptContext* ctx, Js::TypeId arrayType, uint32 length);
        void RecordJsRTAllocateArrayBuffer(Js::ScriptContext* ctx, byte* buff, uint32 size);
        void RecordJsRTAllocateFunction(Js::ScriptContext* ctx, bool isNamed, Js::Var optName);

        //Record GetAndClearException
        void RecordJsRTGetAndClearException(Js::ScriptContext* ctx);

        //Record Object Getters
        void RecordJsRTGetProperty(Js::ScriptContext* ctx, Js::PropertyId pid, Js::Var var);
        void RecordJsRTGetIndex(Js::ScriptContext* ctx, Js::Var index, Js::Var var);
        void RecordJsRTGetOwnPropertyInfo(Js::ScriptContext* ctx, Js::PropertyId pid, Js::Var var);
        void RecordJsRTGetOwnPropertiesInfo(Js::ScriptContext* ctx, bool isGetNames, Js::Var var);

        //Record Object Setters
        void RecordJsRTDefineProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, Js::Var propertyDescriptor);
        void RecordJsRTDeleteProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, bool useStrictRules);
        void RecordJsRTSetPrototype(Js::ScriptContext* ctx, Js::Var var, Js::Var proto);
        void RecordJsRTSetProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, Js::Var val, bool useStrictRules);
        void RecordJsRTSetIndex(Js::ScriptContext* ctx, Js::Var var, Js::Var index, Js::Var val);

        //Record a get info from a typed array
        void RecordJsRTGetTypedArrayInfo(Js::ScriptContext* ctx, bool returnsArrayBuff, Js::Var var);

        //Record a constructor call from JsRT
        void RecordJsRTConstructCall(Js::ScriptContext* ctx, Js::JavascriptFunction* func, uint32 argCount, Js::Var* args);

        //Record callback registration/cancelation
        void RecordJsRTCallbackOperation(Js::ScriptContext* ctx, bool isCancel, bool isRepeating, Js::JavascriptFunction* func, int64 callbackId);

        //Record code parse
        void RecordJsRTCodeParse(Js::ScriptContext* ctx, uint64 bodyCtrId, LoadScriptFlag loadFlag, Js::JavascriptFunction* func, LPCWSTR srcCode, LPCWSTR sourceUri);

        //Record callback of an existing function
        JsRTCallFunctionBeginAction* RecordJsRTCallFunctionBegin(Js::ScriptContext* ctx, int32 rootDepth, int64 hostCallbackId, double beginTime, Js::JavascriptFunction* func, uint32 argCount, Js::Var* args);
        void RecordJsRTCallFunctionEnd(Js::ScriptContext* ctx, int64 matchingBeginTime, bool hasScriptException, bool hasTerminatingException, int32 callbackDepth, double endTime);

        //Replay a sequence of JsRT actions to get to the next event log item
        void ReplayActionLoopStep();

        ////////////////////////////////
        //Emit code and support

        LPCWSTR EmitLogIfNeeded();
        void ParseLogInto();
    };
}

#endif

