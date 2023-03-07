using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;

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

namespace CustomExtensions
{
    public static class StringExtension
    {
        public static int WordCount(this string str)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length;
        }

        public static int WordCount(this string str, int i)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length + i;
        }

        public static int WordCount(this string str, int i, int j)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length + i + j;
        }
    }
}

namespace VSCodeTestExtensionMethods
{
    using CustomExtensions;

    public class MyString
    {
        string s;
        public MyString(string ms)
        {
            s = ms;
        }
    }

    struct MyInt
    {
        int i;
        public MyInt(int mi)
        {
            i = mi;
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            string s = "The quick brown fox jumped over the lazy dog.";
            string st = "first.second";
            List<string> lists = new List<string>();

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

            lists.Add("null");
            lists.Add("first");
            lists.Add("second");
            lists.Add("third");
            lists.Add("fourth");
            string res = lists.ElementAt(1);                                   Label.Breakpoint("BREAK1");

            Label.Checkpoint("bp_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "9", "int", "s.WordCount()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "s.WordCount(1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount(1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount( 1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount(1+1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount( 1+1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount( 1 + 1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount(1,1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "13", "int", "s.WordCount(1+1,1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount(1,1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "s.WordCount(1,1)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "s.WordCount(1,1,1)", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "s.WordCount(\"first\")", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "s.WordCount(1, \"first\")", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "s.WordCount(\"first\", 1)", "error: 0x80070057");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"null\"", "string", "lists.ElementAt(0)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"first\"", "string", "lists.ElementAt(1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"second\"", "string", "lists.ElementAt(2)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"third\"", "string", "lists.ElementAt(3)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"fourth\"", "string", "lists.ElementAt(4)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists.ElemetAt()", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists.ElementAt(1,2)", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists.ElementAt(\"first\")", "error: 0x80070057");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // last checkpoint must provide "finish" as id or empty string ("") as next checkpoint id
            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
