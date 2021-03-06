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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

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

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
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
            Assert.True(VSCodeDebugger.Request(continueRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void CheckErrorAtRequest(string caller_trace, Int64 frameId, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public bool EvalVariable(string caller_trace, int variablesReference, string ExpectedResult, string ExpectedType, string VariableName)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == VariableName) {
                    Assert.Equal(ExpectedType, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(ExpectedResult, Variable.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return true;
                }
            }

            return false;
        }

        public void GetAndCheckChildValue(string caller_trace, Int64 frameId, string ExpectedResult, string ExpectedType, string VariableName, string ChildName)
        {
            int refLocals = GetVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, "Locals");
            int refVar = GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, refLocals, VariableName);

            if (EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, refVar, ExpectedResult, ExpectedType, ChildName))
                return;

            int refVarStatic = GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, refVar, "Static members");
            if (EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, refVarStatic, ExpectedResult, ExpectedType, ChildName))
                return;

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
    }
}

namespace VSCodeTestEvaluate
{
    public struct test_struct1_t
    {
        public test_struct1_t(int x)
        {
            field_i1 = x;
        }
        public int field_i1;
    }

    public class test_class1_t
    {
        public double field_d1 = 7.1;
    }

    public class test_static_class1_t
    {
        ~test_static_class1_t()
        {
            // must be never called in this test!
            throw new System.Exception("test_static_class1_t finalizer called!");
        }

        public int field_i1;
        public test_struct1_t st2 = new test_struct1_t(9);

        public static test_struct1_t st = new test_struct1_t(8);
        public static test_class1_t cl = new test_class1_t();

        public static int static_field_i1 = 5;
        public static int static_property_i2
        { get { return static_field_i1 + 2; }}
    }

    public struct test_static_struct1_t
    {
        public static int static_field_i1 = 4;
        public static float static_field_f1 = 3.0f;
        public int field_i1;

        public static int static_property_i2
        { get { return static_field_i1 + 2; }}
    }

    public class test_this_t
    {
        public int this_i = 1;
        public string this_static_str = "2str";
        public static int this_static_i = 3;

        public void func(int arg_test)
        {
            int this_i = 4;
            int break_line4 = 1;                                                            Label.Breakpoint("BREAK4");

            Label.Checkpoint("this_test", "nested_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK4");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK4");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "501", "int", "arg_test");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_this_t}", "VSCodeTestEvaluate.test_this_t", "this");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "1", "int", "this", "this_i");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "\"2str\"", "string", "this", "this_static_str");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "3", "int", "this", "this_static_i");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "this_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"2str\"", "string", "this_static_str");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "3", "int", "this_static_i");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_this_t.this_i"); // error, cannot be accessed in this way
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_this_t.this_static_str"); // error, cannot be accessed in this way
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "3", "int", "VSCodeTestEvaluate.test_this_t.this_static_i");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }

    public class test_nested
    {
        public static int nested_static_i = 53;
        public int nested_i = 55;

        public class test_nested_1
        {
           // class without members

            public class test_nested_2
            {
                public static int nested_static_i = 253;
                public int nested_i = 255;

                public void func()
                {
                    int break_line5 = 1;                                                            Label.Breakpoint("BREAK5");

                    Label.Checkpoint("nested_test", "base_class_test", (Object context) => {
                        Context Context = (Context)context;
                        Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK5");
                        Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK5");

                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "VSCodeTestEvaluate.test_static_class1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "VSCodeTestEvaluate.test_static_struct1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "53", "int", "VSCodeTestEvaluate.test_nested.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "VSCodeTestEvaluate.test_nested.test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "353", "int", "VSCodeTestEvaluate.test_nested.test_nested_1.test_nested_3.nested_static_i");

                        // nested tests:
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "nested_static_i");
                        // FIXME debugger have wrong behavior and should be fixed first
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "test_static_class1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "test_static_struct1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "53", "int", "test_nested.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested.test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "353", "int", "test_nested_3.nested_static_i");
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested.nested_i"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested.test_nested_1.test_nested_2.nested_i"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_1.test_nested_2.nested_i"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_2.nested_i"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_3.nested_i"); // error, cannot be accessed

                        Context.Continue(@"__FILE__:__LINE__");
                    });
                }
            }
            public class test_nested_3
            {
                public static int nested_static_i = 353;
            }
        }
    }

    abstract public class test_static_parent
    {
        static public int static_i_f_parent = 301;
        static public int static_i_p_parent
        { get { return 302; }}
        public abstract void test();
    }
    public class test_static_child : test_static_parent
    {
        static public int static_i_f_child = 401;
        static public int static_i_p_child
        { get { return 402; }}

        public override void test()
        {
            int break_line5 = 1;                                                            Label.Breakpoint("BREAK7");
        }
    }

    public class test_parent
    {
        public int i_parent = 302;
    }
    public class test_child : test_parent
    {
        public int i_child = 402;
    }

    public struct test_array
    {
        public int i;
    }

    delegate void Lambda(string argVar);

    class Program
    {
        int int_i = 505;
        static test_nested test_nested_static_instance;

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK6");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK7");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK8");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK9");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            decimal dec = 12345678901234567890123456m;
            decimal long_zero_dec = 0.00000000000000000017M;
            decimal short_zero_dec = 0.17M;
            int[] array1 = new int[] { 10, 20, 30, 40, 50 };
            int[,] multi_array2 = { { 101, 102, 103 }, { 104, 105, 106 } };
            test_array[] array2 = new test_array[4];
            array2[0] = new test_array();
            array2[0].i = 201;
            array2[2] = new test_array();
            array2[2].i = 401;
            int break_line1 = 1;                                                                    Label.Breakpoint("BREAK1");

            Label.Checkpoint("values_test", "expression_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "12345678901234567890123456", "decimal", "dec");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0.00000000000000000017", "decimal", "long_zero_dec");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0.17", "decimal", "short_zero_dec");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{int[5]}", "int[]", "array1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "array1[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "30", "int", "array1[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "50", "int", "array1[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "array1[ 0]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "30", "int", "array1[2 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "50", "int", "array1 [ 4 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{int[2, 3]}", "int[,]", "multi_array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multi_array2[0,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multi_array2[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multi_array2[ 0 , 0 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multi_array2  [ 1,1 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array[4]}", "VSCodeTestEvaluate.test_array[]", "array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array}", "VSCodeTestEvaluate.test_array", "array2[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "201", "int", "array2[0].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "201", "int", "array2   [   0   ]   .   i"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array}", "VSCodeTestEvaluate.test_array", "array2[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "array2[2].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "array2  [  2  ]  .  i"); // check spaces

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "this.int_i"); // error, Main is static method that don't have "this"
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "int_i"); // error, don't have "this" (no object of type "Program" was created)
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "not_declared"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "array1[]"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "multi_array2[]"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "multi_array2[,]"); // error, no such variable

                Context.Continue(@"__FILE__:__LINE__");
            });


            int int_i1 = 5;
            int int_i2 = 5;
            string str_s1 = "one";
            string str_s2 = "two";
            int break_line2 = 1;                                                                    Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test", "static_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "int_i1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "int_i1 + int_i2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"onetwo\"", "", "\"one\" + \"two\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"onetwo\"", "", "str_s1 + \"two\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"onetwo\"", "", "str_s1 + str_s2");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 + not_var"); // error

                Context.Continue(@"__FILE__:__LINE__");
            });

            // switch to separate scope, in case `cl` constructor called by some reason, GC will able to care
            test_SuppressFinalize();
            void test_SuppressFinalize()
            {
                test_static_class1_t cl;
                test_static_struct1_t st;
                st.field_i1 = 2;
                int break_line3 = 1;                                                                Label.Breakpoint("BREAK3");
            }
            // in this way we check that finalizer was not called by GC for `cl`
            GC.Collect();

            Label.Checkpoint("static_test", "this_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK3");

                // test class fields/properties via local variable
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "5", "int", "cl", "static_field_i1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "7", "int", "cl", "static_property_i2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "cl.st"); // error CS0176: Member 'test_static_class1_t.st' cannot be accessed with an instance reference; qualify it with a type name instead
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "cl.cl"); // error CS0176: Member 'test_static_class1_t.cl' cannot be accessed with an instance reference; qualify it with a type name instead
                // test struct fields via local variable
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "4", "int", "st", "static_field_i1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "3", "float", "st", "static_field_f1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "6", "int", "st", "static_property_i2");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", frameId, "2", "int", "st", "field_i1");
                // test direct eval for class and struct static fields/properties
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "VSCodeTestEvaluate.test_static_class1_t.static_field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "test_static_class1_t.static_field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "VSCodeTestEvaluate . test_static_class1_t  .  static_field_i1  "); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "VSCodeTestEvaluate.test_static_class1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "test_static_class1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "VSCodeTestEvaluate.test_static_struct1_t.static_field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "3", "float", "VSCodeTestEvaluate.test_static_struct1_t.static_field_f1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "VSCodeTestEvaluate.test_static_struct1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "test_static_struct1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "8", "int", "VSCodeTestEvaluate.test_static_class1_t.st.field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7.1", "double", "VSCodeTestEvaluate.test_static_class1_t.cl.field_d1");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.not_declared"); // error, no such field in class
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_struct1_t.not_declared"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.st.not_declared"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.cl.not_declared"); // error, no such field in class

                Context.Continue(@"__FILE__:__LINE__");
            });

            test_this_t test_this = new test_this_t();
            test_this.func(501);

            test_nested.test_nested_1.test_nested_2 test_nested = new test_nested.test_nested_1.test_nested_2();
            test_nested.func();

            test_nested_static_instance  = new test_nested();

            test_child child = new test_child();

            int break_line6 = 1;                                                                     Label.Breakpoint("BREAK6");

            Label.Checkpoint("base_class_test", "override_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK6");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK6");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "402", "int", "child.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "302", "int", "child.i_parent");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "VSCodeTestEvaluate.test_static_child.static_i_f_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "402", "int", "VSCodeTestEvaluate.test_static_child.static_i_p_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "301", "int", "VSCodeTestEvaluate.test_static_child.static_i_f_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "302", "int", "VSCodeTestEvaluate.test_static_child.static_i_p_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_static_child}", "VSCodeTestEvaluate.test_static_child", "VSCodeTestEvaluate.test_static_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "Program.test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "VSCodeTestEvaluate.Program.test_nested_static_instance.nested_i");

                Context.Continue(@"__FILE__:__LINE__");
            });

            test_static_parent base_child = new test_static_child();
            base_child.test();

            Label.Checkpoint("override_test", "lambda_test1", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK7");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK7");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "static_i_f_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "402", "int", "static_i_p_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "301", "int", "static_i_f_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "302", "int", "static_i_p_parent");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test lambda.

            string mainVar = "mainVar";

            Lambda lambda1 = (argVar) => {
                string localVar = "localVar";

                Label.Checkpoint("lambda_test1", "lambda_test2", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK8");
                    Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK8");

                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"mainVar\"", "string", "mainVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"localVar\"", "string", "localVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"argVar\"", "string", "argVar");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                Console.WriteLine(mainVar);                                                             Label.Breakpoint("BREAK8");
            };

            lambda1("argVar");

            Lambda lambda2 = (argVar) => {
                string localVar = "localVar";

                Label.Checkpoint("lambda_test2", "finish", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK9");
                    Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK9");

                    Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "mainVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"localVar\"", "string", "localVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"argVar\"", "string", "argVar");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                int break_line6 = 1;                                                                     Label.Breakpoint("BREAK9");
            };

            lambda2("argVar");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
