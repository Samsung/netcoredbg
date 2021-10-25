using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void PrepareStart(string caller_trace)
        {
            InitializeRequest initializeRequest = new InitializeRequest();
            initializeRequest.arguments.clientID = "vscode";
            initializeRequest.arguments.clientName = "Visual Studio Code";
            initializeRequest.arguments.adapterID = "coreclr";
            initializeRequest.arguments.pathFormat = "path";
            initializeRequest.arguments.linesStartAt1 = true;
            initializeRequest.arguments.columnsStartAt1 = true;
            initializeRequest.arguments.supportsVariableType = true;
            initializeRequest.arguments.supportsVariablePaging = true;
            initializeRequest.arguments.supportsRunInTerminalRequest = true;
            initializeRequest.arguments.locale = "en-us";
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = ControlInfo.TargetAssemblyPath;
            launchRequest.arguments.cwd = "";
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.justMyCode = true; // Explicitly enable JMC for this test.
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void PrepareEnd(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasEntryPointHit(string caller_trace)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
        {
            bool wasExited = false;
            bool wasTerminated = false;

            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "exited")) {
                    wasExited = true;
                    ExitedEvent exitedEvent = JsonConvert.DeserializeObject<ExitedEvent>(resJSON);
                }
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated")) {
                    wasTerminated = true;
                }
                // we don't check exit code here, since Windows and Linux provide different exit code in case of unhandled exception
                if (wasExited && wasTerminated)
                    return true;

                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AbortExecution(string caller_trace)
        {
            TerminateRequest terminateRequest = new TerminateRequest();
            terminateRequest.arguments = new TerminateArguments();
            terminateRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(terminateRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddBreakpoint(string caller_trace, string bpName, string Condition = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition));
            BreakpointLines.Add(lbp.NumLine);
        }

        public void SetBreakpoints(string caller_trace)
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = BreakpointSourceName;
            // NOTE this code works only with one source file
            setBreakpointsRequest.arguments.source.path = ControlInfo.SourceFilesPath;
            setBreakpointsRequest.arguments.lines.AddRange(BreakpointLines);
            setBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            setBreakpointsRequest.arguments.sourceModified = false;
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpName)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
                return;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void ResetExceptionBreakpoints()
        {
            ExceptionFilterAll = false;
            ExceptionFilterUserUnhandled = false;
            ExceptionFilterAllOptions = null;
            ExceptionFilterUserUnhandledOptions = null;
        }

        public void AddExceptionBreakpointFilterAll()
        {
            ExceptionFilterAll = true;
        }

        public void AddExceptionBreakpointFilterUserUnhandled()
        {
            ExceptionFilterUserUnhandled = true;
        }

        public void AddExceptionBreakpointFilterAllWithOptions(string options)
        {
            ExceptionFilterAllOptions = new ExceptionFilterOptions();
            ExceptionFilterAllOptions.filterId = "all";
            ExceptionFilterAllOptions.condition = options;

        }

        public void AddExceptionBreakpointFilterUserUnhandledWithOptions(string options)
        {
            ExceptionFilterUserUnhandledOptions = new ExceptionFilterOptions();
            ExceptionFilterUserUnhandledOptions.filterId = "user-unhandled";
            ExceptionFilterUserUnhandledOptions.condition = options;
        }

        public void SetExceptionBreakpoints(string caller_trace)
        {
            SetExceptionBreakpointsRequest setExceptionBreakpointsRequest = new SetExceptionBreakpointsRequest();
            if (ExceptionFilterAll)
                setExceptionBreakpointsRequest.arguments.filters.Add("all");
            if (ExceptionFilterUserUnhandled)
                setExceptionBreakpointsRequest.arguments.filters.Add("user-unhandled");

            if (ExceptionFilterAllOptions != null || ExceptionFilterUserUnhandledOptions != null)
                setExceptionBreakpointsRequest.arguments.filterOptions = new List<ExceptionFilterOptions>();
            if (ExceptionFilterAllOptions != null)
                setExceptionBreakpointsRequest.arguments.filterOptions.Add(ExceptionFilterAllOptions);
            if (ExceptionFilterUserUnhandledOptions != null)
                setExceptionBreakpointsRequest.arguments.filterOptions.Add(ExceptionFilterUserUnhandledOptions);

            Assert.True(VSCodeDebugger.Request(setExceptionBreakpointsRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void TestExceptionInfo(string caller_trace, string excCategory, string excMode, string excName)
        {
            ExceptionInfoRequest exceptionInfoRequest = new ExceptionInfoRequest();
            exceptionInfoRequest.arguments.threadId = threadId;
            var ret = VSCodeDebugger.Request(exceptionInfoRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            ExceptionInfoResponse exceptionInfoResponse =
                JsonConvert.DeserializeObject<ExceptionInfoResponse>(ret.ResponseStr);

            if (exceptionInfoResponse.body.breakMode == excMode
                && exceptionInfoResponse.body.exceptionId == excCategory + "/" + excName
                && exceptionInfoResponse.body.details.fullTypeName == excName)
                return;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void TestInnerException(string caller_trace, int innerLevel, string excName, string excMessage)
        {
            ExceptionInfoRequest exceptionInfoRequest = new ExceptionInfoRequest();
            exceptionInfoRequest.arguments.threadId = threadId;
            var ret = VSCodeDebugger.Request(exceptionInfoRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            ExceptionInfoResponse exceptionInfoResponse =
                JsonConvert.DeserializeObject<ExceptionInfoResponse>(ret.ResponseStr);


            ExceptionDetails exceptionDetails = exceptionInfoResponse.body.details.innerException[0];
            for (int i = 0; i < innerLevel; ++i)
                exceptionDetails = exceptionDetails.innerException[0];

            if (exceptionDetails.fullTypeName == excName
                && exceptionDetails.message == excMessage)
                return;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExceptionBreakpointHit(string caller_trace, string bpName, string excCategory, string excMode, string excName)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "exception")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
            {
                TestExceptionInfo(@"__FILE__:__LINE__"+"\n"+caller_trace, excCategory, excMode, excName);
                return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExceptionBreakpointHitInExternalCode(string caller_trace, string excCategory, string excMode, string excName, string extFrame)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "exception")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].name == extFrame)
            {
                TestExceptionInfo(@"__FILE__:__LINE__"+"\n"+caller_trace, excCategory, excMode, excName);
                return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
        bool ExceptionFilterAll = false;
        bool ExceptionFilterUserUnhandled = false;
        ExceptionFilterOptions ExceptionFilterAllOptions = null;
        ExceptionFilterOptions ExceptionFilterUserUnhandledOptions = null;
    }
}

namespace VSCodeTestExceptionBreakpoint
{
    class inside_user_code
    {
        static public void throw_Exception()
        {
            throw new System.Exception();                                                          Label.Breakpoint("bp3");
        }

        static public void throw_NullReferenceException()
        {
            throw new System.NullReferenceException();                                             Label.Breakpoint("bp4");
        }

        static public void throw_Exception_with_catch()
        {
            try {
                throw new System.Exception();                                                      Label.Breakpoint("bp1");
            } catch (Exception e) {}
        }

        static public void throw_Exception_NullReferenceException_with_catch()
        {
            try {
                throw new System.Exception();
            } catch {}

            try {
                throw new System.NullReferenceException();                                         Label.Breakpoint("bp2");
            } catch {}
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class outside_user_code
    {
        static public void throw_Exception()
        {
            throw new System.Exception();
        }

        static public void throw_NullReferenceException()
        {
            throw new System.NullReferenceException();
        }

        static public void throw_Exception_with_catch()
        {
            try {
                throw new System.Exception();
            } catch {}
        }

        static public void throw_Exception_NullReferenceException_with_catch()
        {
            try {
                throw new System.Exception();
            } catch {}

            try {
                throw new System.NullReferenceException();
            } catch {}
        }
    }

    class inside_user_code_wrapper
    {
        static public void call(Action callback)
        {
            callback();
        }

        static public void call_with_catch(Action callback)
        {
            try {
                callback();
            } catch {};
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class outside_user_code_wrapper
    {
        static public void call(Action callback)
        {
            callback();
        }

        static public void call_with_catch(Action callback)
        {
            try {
                callback();
            } catch {};
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_all", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_3");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_6");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_7");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_8");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_9");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_10");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_11");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_12");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_13");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_14");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_15");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_16");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_test_17");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAll();
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "all"

            for (int i = 0; i < 2; ++i)
            {
                inside_user_code.throw_Exception_with_catch();                                     Label.Breakpoint("bp_test_1");
                try {
                    outside_user_code.throw_Exception();                                           Label.Breakpoint("bp_test_2");
                } catch {};
                outside_user_code.throw_Exception_with_catch();                                    Label.Breakpoint("bp_test_3");
            }

            Label.Checkpoint("test_all", "test_all_empty_options", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_1");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp1", "CLR", "always", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_2");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.Exception", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception()");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_3");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.Exception", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception_with_catch()");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAllWithOptions("");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "all" with empty options

            Label.Checkpoint("test_all_empty_options", "test_all_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_1");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp1", "CLR", "always", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_2");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.Exception", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception()");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_3");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.Exception", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception_with_catch()");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAllWithOptions("System.NullReferenceException");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "all" with options "System.NullReferenceException" ("all" for "System.NullReferenceException" only)

            for (int i = 0; i < 2; ++i)
            {
                inside_user_code.throw_Exception_NullReferenceException_with_catch();              Label.Breakpoint("bp_test_4");
                outside_user_code.throw_Exception_NullReferenceException_with_catch();             Label.Breakpoint("bp_test_5");
                try {
                    outside_user_code.throw_Exception();
                } catch {};
                try {
                    outside_user_code.throw_NullReferenceException();                              Label.Breakpoint("bp_test_6");
                } catch {};
            }

            Label.Checkpoint("test_all_concrete_exception", "test_all_except_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_4");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp2", "CLR", "always", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_5");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.NullReferenceException", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception_NullReferenceException_with_catch()");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_6");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.NullReferenceException", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_NullReferenceException()");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAllWithOptions("!System.Exception");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "all" with options "!System.Exception" ("all" for all except "System.Exception")

            Label.Checkpoint("test_all_except_concrete_exception", "test_user_unhandled", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_4");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp2", "CLR", "always", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_5");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.NullReferenceException", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_Exception_NullReferenceException_with_catch()");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_6");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHitInExternalCode(@"__FILE__:__LINE__", "CLR", "always", "System.NullReferenceException", "VSCodeTestExceptionBreakpoint.outside_user_code.throw_NullReferenceException()");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterUserUnhandled();
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "user-unhandled"
            // Must emit break event only in case catch block outside of user code, but "throw" inside user code.

            for (int i = 0; i < 2; ++i)
            {
                inside_user_code.throw_Exception_with_catch();
                try {
                    outside_user_code.throw_Exception();
                } catch {};
                outside_user_code.throw_Exception_with_catch();

                try {
                    outside_user_code_wrapper.call(inside_user_code.throw_Exception);
                } catch {};
                try {
                    outside_user_code_wrapper.call(outside_user_code.throw_Exception);
                } catch {};
                outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);       Label.Breakpoint("bp_test_7");
                outside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);      Label.Breakpoint("bp_test_8");

                try {
                    inside_user_code_wrapper.call(outside_user_code.throw_Exception);
                } catch {};
                try {
                    inside_user_code_wrapper.call(inside_user_code.throw_Exception);
                } catch {};
                inside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);
                inside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);
            }

            Label.Checkpoint("test_user_unhandled", "test_user_unhandled_empty_options", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_7");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "userUnhandled", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_8");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterUserUnhandledWithOptions("");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "user-unhandled" with empty options

            Label.Checkpoint("test_user_unhandled_empty_options", "test_user_unhandled_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_7");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "userUnhandled", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_8");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterUserUnhandledWithOptions("System.NullReferenceException");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "user-unhandled" with options "System.NullReferenceException" ("user-unhandled" for "System.NullReferenceException" only)

            for (int i = 0; i < 2; ++i)
            {
                outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);
                outside_user_code_wrapper.call_with_catch(inside_user_code.throw_NullReferenceException);   Label.Breakpoint("bp_test_9");
            }

            Label.Checkpoint("test_user_unhandled_concrete_exception", "test_user_unhandled_except_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_9");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "userUnhandled", "System.NullReferenceException");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterUserUnhandledWithOptions("!System.Exception");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test filter "user-unhandled" with options "!System.Exception" ("user-unhandled" for all except "System.Exception")

            Label.Checkpoint("test_user_unhandled_except_concrete_exception", "test_vscode_1", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_9");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "userUnhandled", "System.NullReferenceException");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAllWithOptions("System.NullReferenceException");
                Context.AddExceptionBreakpointFilterUserUnhandled();
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test VSCode add multiple breakpoints (filter + filter options)

            for (int i = 0; i < 3; ++i)
            {
                outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);                Label.Breakpoint("bp_test_10");
                outside_user_code_wrapper.call_with_catch(inside_user_code.throw_NullReferenceException);   Label.Breakpoint("bp_test_11");
                Console.WriteLine("end");                                                                   Label.Breakpoint("bp_test_12");
            }

            Label.Checkpoint("test_vscode_1", "test_vscode_2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_10");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "userUnhandled", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_11");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "always", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "userUnhandled", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_12");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAll();
                Context.AddExceptionBreakpointFilterUserUnhandled();
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test VSCode add multiple breakpoints (both filters)

            Label.Checkpoint("test_vscode_2", "test_vscode_3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_10");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "always", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "userUnhandled", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_11");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "always", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "CLR", "userUnhandled", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_12");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAllWithOptions("!System.NullReferenceException System.ArgumentNullException System.ArgumentOutOfRangeException");
                Context.AddExceptionBreakpointFilterUserUnhandledWithOptions("System.ArgumentNullException System.Exception System.ArgumentOutOfRangeException");
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test VSCode add multiple breakpoints (both filter options)

            Label.Checkpoint("test_vscode_3", "test_vscode_inner", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_10");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "always", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "CLR", "userUnhandled", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_11");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_12");

                Context.ResetExceptionBreakpoints();
                Context.AddExceptionBreakpointFilterAll();
                Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test VSCode inner exception (test ExceptionInfo for proper inner exception info)

            try {
                throw new Exception("Message1");                                                        Label.Breakpoint("bp_test_13");
            } catch (Exception e1) {
                try {
                    throw new NullReferenceException("Message2", e1);                                   Label.Breakpoint("bp_test_14");
                } catch (Exception e2) {
                    try {
                        throw new ArgumentOutOfRangeException("Message3", e2);                          Label.Breakpoint("bp_test_15");
                    } catch {}
                }
            }
            Console.WriteLine("end");                                                                   Label.Breakpoint("bp_test_16");

            Label.Checkpoint("test_vscode_inner", "test_unhandled", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_13");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_13", "CLR", "always", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_14");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_14", "CLR", "always", "System.NullReferenceException");
                Context.TestInnerException(@"__FILE__:__LINE__", 0, "System.Exception", "Message1");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_15");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_15", "CLR", "always", "System.ArgumentOutOfRangeException");
                Context.TestInnerException(@"__FILE__:__LINE__", 0, "System.NullReferenceException", "Message2");
                Context.TestInnerException(@"__FILE__:__LINE__", 1, "System.Exception", "Message1");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_16");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "unhandled"

            throw new System.ArgumentOutOfRangeException();                                             Label.Breakpoint("bp_test_17");

            Label.Checkpoint("test_unhandled", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_17");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_17", "CLR", "always", "System.ArgumentOutOfRangeException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_17", "CLR", "unhandled", "System.ArgumentOutOfRangeException");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                // At this point debugger stops at unhandled exception, no reason continue process, abort execution.
                Context.AbortExecution(@"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
