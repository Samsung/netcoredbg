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

        public void SetExpression(string caller_trace, Int64 frameId, string Expression, string Value)
        {
            SetExpressionRequest setExpressionRequest = new SetExpressionRequest();
            setExpressionRequest.arguments.expression = Expression;
            setExpressionRequest.arguments.value = Value;
            Assert.True(VSCodeDebugger.Request(setExpressionRequest).Success, @"__FILE__:__LINE__");
        }

        public void SetVariable(string caller_trace, Int64 frameId, int variablesReference, string Name, string Value, bool ignoreCheck = false)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            Assert.True(VSCodeDebugger.Request(setVariableRequest).Success, @"__FILE__:__LINE__");

            if (ignoreCheck)
                return;

            string realValue = GetVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, Value);
            EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, variablesReference, "", Name, realValue);
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

        public int GetVariablesReference(string caller_trace, Int64 frameId, string ScopeName)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    return Scope.variablesReference == null ? 0 : (int)Scope.variablesReference;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public int GetChildVariablesReference(string caller_trace, int VariablesReference, string VariableName)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = VariablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == VariableName)
                    return Variable.variablesReference;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public string GetVariable(string caller_trace, Int64 frameId, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            var fixedVal = evaluateResponse.body.result;
            if (evaluateResponse.body.type == "char")
            {
                int foundStr = fixedVal.IndexOf(" ");
                if (foundStr >= 0)
                    fixedVal = fixedVal.Remove(foundStr);
            }
            return fixedVal;
        }

        public void EvalVariable(string caller_trace, int variablesReference, string Type, string Name, string Value)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == Name) {
                    if (Type != "")
                        Assert.Equal(Type, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);

                    var fixedVal = Variable.value;
                    if (Variable.type == "char")
                    {
                        int foundStr = fixedVal.IndexOf(" ");
                        if (foundStr >= 0)
                            fixedVal = fixedVal.Remove(foundStr);
                    }

                    Assert.Equal(Value, fixedVal, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestGeneric
{
    class Program
    {
        public class TestGeneric<T,U>
        {

            public T test1(T arg1)
            {
                return arg1;
            }

            public W test2<W>(W arg2)
            {
                return arg2;
            }

            static public T static_test1(T arg1)
            {
                return arg1;
            }

            static public W static_test2<W>(W arg2)
            {
                return arg2;
            }

            public T i1;
            public U s1;

            public T p_i1
            { get; set; }

            public U p_s1
            { get; set; }

            public static T static_i1;
            public static U static_s1;

            public static T static_p_i1
            { get; set; }

            public static U static_p_s1
            { get; set; }

            public void test_func()
            {
                Console.WriteLine("test_func()");                                                            Label.Breakpoint("BREAK2");

                Label.Checkpoint("test_func", "test_set_value", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");
                    Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK2");

                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestGeneric.Program.TestGeneric<int, string>}", "VSCodeTestGeneric.Program.TestGeneric<int, string>", "this");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "123", "int", "i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string1\"", "string", "s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "234", "int", "p_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string2\"", "string", "p_s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "345", "int", "static_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string3\"", "string", "static_s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "456", "int", "static_p_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string4\"", "string", "static_p_s1");

                    // FIXME
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "test1(5)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "test2<int>(10)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "static_test1(15)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "20", "int", "static_test2<int>(20)");

                    // Test set value.

                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "i1", "55");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "s1", "\"changed1\"");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "p_i1", "66");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "p_s1", "\"changed2\"");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "static_i1", "77");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "static_s1", "\"changed3\"");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "static_p_i1", "88");
                    Context.SetExpression(@"__FILE__:__LINE__", frameId, "static_p_s1", "\"changed4\"");

                    int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                    int setReference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference, "this");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "i1", "55555");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "s1", "\"changed_string111\"");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "p_i1", "66666");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "p_s1", "\"changed_string222\"");

                    int setStaticReference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", setReference, "Static members");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_i1", "77777");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_s1", "\"changed_string333\"");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_p_i1", "88888");
                    Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_p_s1", "\"changed_string444\"");

                    Context.Continue(@"__FILE__:__LINE__");
                });
            }
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_main", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            TestGeneric<int,string> ttt = new TestGeneric<int,string>();
            ttt.i1 = 123;
            ttt.s1 = "test_string1";
            ttt.p_i1 = 234;
            ttt.p_s1 = "test_string2";
            TestGeneric<int,string>.static_i1 = 345;
            TestGeneric<int,string>.static_s1 = "test_string3";
            TestGeneric<int,string>.static_p_i1 = 456;
            TestGeneric<int,string>.static_p_s1 = "test_string4";

            ttt.test_func();                                                                                 Label.Breakpoint("BREAK1");

            Label.Checkpoint("test_main", "test_func", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestGeneric.Program.TestGeneric<int, string>}", "VSCodeTestGeneric.Program.TestGeneric<int, string>", "ttt");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "123", "int", "ttt.i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string1\"", "string", "ttt.s1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "234", "int", "ttt.p_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string2\"", "string", "ttt.p_s1");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "ttt.static_i1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "ttt.static_s1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "ttt.static_p_i1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "ttt.static_p_s1", "error");

                // FIXME debugger must be fixed first
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "345", "int", "TestGeneric<int, string>.static_i1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string3\"", "string", "TestGeneric<int, string>.static_s1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "456", "int", "TestGeneric<int, string>.static_p_i1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test_string4\"", "string", "TestGeneric<int, string>.static_p_s1");

                // FIXME debugger must be fixed first
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "ttt.test1(5)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "ttt.test2<int>(10)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "TestGeneric<int,string>.static_test1(15)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "20", "int", "TestGeneric<int,string>.static_test2<int>(20)");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test set value");                                                             Label.Breakpoint("BREAK3");

            Label.Checkpoint("test_set_value", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK3");

                Context.SetExpression(@"__FILE__:__LINE__", frameId, "ttt.i1", "555");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "ttt.s1", "\"changed_string1\"");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "ttt.p_i1", "666");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "ttt.p_s1", "\"changed_string2\"");

                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int setReference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference, "ttt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "i1", "5555");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "s1", "\"changed_string11\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "p_i1", "6666");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setReference, "p_s1", "\"changed_string22\"");

                int setStaticReference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", setReference, "Static members");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_i1", "7777");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_s1", "\"changed_string33\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_p_i1", "8888");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setStaticReference, "static_p_s1", "\"changed_string44\"");

                // FIXME debugger must be fixed first
                //Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestGeneric<int,string>.static_i1", "777");
                //Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestGeneric<int,string>.static_s1", "\"changed_string3\"");

                // FIXME debugger must be fixed first
                //Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestGeneric<int,string>.static_p_i1", "888");
                //Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestGeneric<int,string>.static_p_s1", "\"changed_string4\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
