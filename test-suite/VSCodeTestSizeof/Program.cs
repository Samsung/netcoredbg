using System;
using System.IO;
using System.Collections.Generic;

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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = ControlInfo.TargetAssemblyPath;
            launchRequest.arguments.cwd = "";
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void PrepareEnd(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
        {
            bool wasExited = false;
            int ?exitCode = null;
            bool wasTerminated = false;

            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "exited")) {
                    wasExited = true;
                    ExitedEvent exitedEvent = JsonConvert.DeserializeObject<ExitedEvent>(resJSON);
                    exitCode = exitedEvent.body.exitCode;
                }
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated")) {
                    wasTerminated = true;
                }
                if (wasExited && exitCode == 0 && wasTerminated)
                    return true;

                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddBreakpoint(string caller_trace, string bpName, string Condition = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
        }

        public Int64 DetectFrameId(string caller_trace, string bpName)
        {
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
                return stackTraceResponse.body.stackFrames[0].id;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetAndCheckValue(string caller_trace, Int64 frameId, string ExpectedResult, string ExpectedType, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.Equal(ExpectedResult, evaluateResponse.body.result, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedType, evaluateResponse.body.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CheckErrorAtRequest(string caller_trace, Int64 frameId, string Expression, string errMsgStart)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.True(evaluateResponse.message.StartsWith(errMsgStart), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetResultAsString(string caller_trace, Int64 frameId, string expr, out string strRes)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = expr;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            EvaluateResponse evaluateResponse = JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);
            strRes = evaluateResponse.body.result;
        }

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestSizeof
{
    public struct Point
    {
        public Point(byte tag, decimal x, decimal y) => (Tag, X, Y) = (tag, x, y);

        public decimal X { get; }
        public decimal Y { get; }
        public byte Tag { get; }
    }

    class Program
    {
        static void Main(string[] args)
        {
            // first checkpoint (initialization) must provide "init" as id
            Label.Checkpoint("init", "bp_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            int a = 10;
            TestStruct tc = new TestStruct(a, 11);
            string str1 = "string1";
            uint c;
            unsafe { c = (uint)sizeof(Point); }
            uint d = c;                                                   Label.Breakpoint("BREAK1");

            Label.Checkpoint("bp_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(bool).ToString(), "uint", "sizeof(bool)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(byte).ToString(), "uint", "sizeof(byte)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(sbyte).ToString(), "uint", "sizeof(sbyte)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(char).ToString(), "uint", "sizeof(char)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(int).ToString(), "uint", "sizeof(int)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(uint).ToString(), "uint", "sizeof(uint)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(long).ToString(), "uint", "sizeof(long)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(ulong).ToString(), "uint", "sizeof(ulong)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(float).ToString(), "uint", "sizeof(float)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(double).ToString(), "uint", "sizeof(double)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(decimal).ToString(), "uint", "sizeof(decimal)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Boolean).ToString(), "uint", "sizeof(System.Boolean)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Byte).ToString(), "uint", "sizeof(System.Byte)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Char).ToString(), "uint", "sizeof(System.Char)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Decimal).ToString(), "uint", "sizeof(System.Decimal)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Double).ToString(), "uint", "sizeof(System.Double)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Int16).ToString(), "uint", "sizeof(System.Int16)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Int32).ToString(), "uint", "sizeof(System.Int32)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Int64).ToString(), "uint", "sizeof(System.Int64)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.SByte).ToString(), "uint", "sizeof(System.SByte)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.Single).ToString(), "uint", "sizeof(System.Single)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.UInt16).ToString(), "uint", "sizeof(System.UInt16)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.UInt32).ToString(), "uint", "sizeof(System.UInt32)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, sizeof(System.UInt64).ToString(), "uint", "sizeof(System.UInt64)");
                string ss1;
                Context.GetResultAsString(@"__FILE__:__LINE__", frameId, "sizeof(Point)", out ss1);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, ss1, "uint", "c");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(a)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(tc)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(str1)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(abcd)", "error: The type or namespace name");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(Program)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "sizeof(tc.a)", "");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // last checkpoint must provide "finish" as id or empty string ("") as next checkpoint id
            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        struct TestStruct
        {
            public int a;
            public int b;

            public TestStruct(int x, int y)
            {
                a = x;
                b = y;
            }
        }
    }
}
