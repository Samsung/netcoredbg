using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading.Tasks;

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

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void TestExceptionStackTrace(string caller_trace,  string[] stacktrace, int num)
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

            for (int i = 0; i < num; i++)
            {
                Breakpoint bp = ControlInfo.Breakpoints[stacktrace[i]];
                Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                var lbp = (LineBreakpoint)bp;

                if (lbp.FileName != stackTraceResponse.body.stackFrames[i].source.name ||
                    ControlInfo.SourceFilesPath != stackTraceResponse.body.stackFrames[i].source.path ||
                    lbp.NumLine != stackTraceResponse.body.stackFrames[i].line)
                {
                    throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
                }
            }
            return;
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
    class Program
    {
        static void Abc()
        {
            throw new Exception();                                                  Label.Breakpoint("throwexception");
        }

        static async Task Main(string[] args)
        {
            Label.Checkpoint("init", "test_unhandled", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "unhandled"
            await Task.Yield();
            Abc();                                                                  Label.Breakpoint("callabc");

            Label.Checkpoint("test_unhandled", "finish", (Object context) => {
                Context Context = (Context)context;
                string[] stacktrace = {"throwexception", "callabc"};
                Context.TestExceptionStackTrace(@"__FILE__:__LINE__", stacktrace, 2);
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
