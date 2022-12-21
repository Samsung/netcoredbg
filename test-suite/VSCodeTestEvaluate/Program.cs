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

        public void GetAndCheckValue(string caller_trace, Int64 frameId, string ExpectedResult1, string ExpectedResult2, string ExpectedType, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.True(ExpectedResult1 == evaluateResponse.body.result ||
                        ExpectedResult2 == evaluateResponse.body.result, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void AddExceptionBreakpointFilterAllWithOptions(string options)
        {
            ExceptionFilterAllOptions = new ExceptionFilterOptions();
            ExceptionFilterAllOptions.filterId = "all";
            ExceptionFilterAllOptions.condition = options;
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
        bool ExceptionFilterAll = false;
        bool ExceptionFilterUserUnhandled = false;
        ExceptionFilterOptions ExceptionFilterAllOptions = null;
        ExceptionFilterOptions ExceptionFilterUserUnhandledOptions = null;
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
        static int Calc1()
        {
            return 15;
        }

        int Calc2()
        {
            return 16;
        }

        void TestVoidReturn()
        {
            Console.WriteLine("test void return");
        }

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

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_this_t.this_i", "error:"); // error, cannot be accessed in this way
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_this_t.this_static_str", "error:"); // error, cannot be accessed in this way
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "3", "int", "VSCodeTestEvaluate.test_this_t.this_static_i");

                // Test method calls.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "Calc1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "test_this_t.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "this.Calc1()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "16", "int", "Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "16", "int", "this.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_this_t.Calc2()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"VSCodeTestEvaluate.test_this_t\"", "string", "ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"VSCodeTestEvaluate.test_this_t\"", "string", "this.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Expression has been evaluated and has no value", "void", "TestVoidReturn()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "TestVoidReturn() + 1", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 + TestVoidReturn()", "error CS0019");

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
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "test_static_class1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "test_static_struct1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "53", "int", "test_nested.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested.test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "253", "int", "test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "353", "int", "test_nested_3.nested_static_i");
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested.test_nested_1.test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_1.test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_nested_3.nested_i", "error:"); // error, cannot be accessed

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

        public virtual int GetNumber()
        {
            return 10;
        }
    }
    public class test_child : test_parent
    {
        public int i_child = 402;

        public override int GetNumber()
        {
            return 11;
        }
    }

    public class conditional_access1
    {
        public int i = 555;
        public int index = 1;
    }

    public class conditional_access2
    {
        public conditional_access1 member = new conditional_access1();
    }

    public class conditional_access3
    {
        public conditional_access1 member;
    }

    public class object_with_array
    {
        public int[] array = new int[] { 551, 552, 553 };
    }

    public struct test_array
    {
        public int i;
    }

    delegate void Lambda(string argVar);

    public class MethodCallTest1
    {
        public static int Calc1()
        {
            return 5;
        }

        public int Calc2()
        {
            return 6;
        }
    }

    public class coalesce_test_A
    {
        public string A = "A";
    }
    public class coalesce_test_B
    {
        public string B = "B";
    }

    public class MethodCallTest2
    {
        public static MethodCallTest1 member1 = new MethodCallTest1();
        public MethodCallTest1 nullMember;

    }

    public class MethodCallTest3
    {
        public int Calc1(int i)
        {
            return i + 1;
        }

        public decimal Calc1(decimal d)
        {
            return d + 1;
        }

        public float Calc2(float f1, float f2, float f3)
        {
            return f1 * 100 + f2 * 10 + f3;
        }

        public int Calc2(int i1, int i2, int i3)
        {
            return i1 * 100 + i2 * 10 + i3;
        }

        public int Calc2(int i1, int i2, float f3)
        {
            return i1 * 100 + i2 * 10 + (int)f3;
        }
    }

    enum enumUnary_t
    {
        ONE,
        TWO
    };

    public class TestOperators1
    {
        public int data;
        public TestOperators1(int data_)
        {
            data = data_;
        }

        public static implicit operator TestOperators1(int value) => new TestOperators1(value);

        public static int operator +(TestOperators1 d1, int d2) => 55;
        public static int operator +(int d1, TestOperators1 d2) => 66;

        public static int operator ~(TestOperators1 d1) => ~d1.data;
        public static bool operator !(TestOperators1 d1) => true;
        public static int operator +(TestOperators1 d1, TestOperators1 d2) => d1.data + d2.data;
        public static int operator -(TestOperators1 d1, TestOperators1 d2) => d1.data - d2.data;
        public static int operator *(TestOperators1 d1, TestOperators1 d2) => d1.data * d2.data;
        public static int operator /(TestOperators1 d1, TestOperators1 d2) => d1.data / d2.data;
        public static int operator %(TestOperators1 d1, TestOperators1 d2) => d1.data % d2.data;
        public static int operator ^(TestOperators1 d1, TestOperators1 d2) => d1.data ^ d2.data;
        public static int operator &(TestOperators1 d1, TestOperators1 d2) => d1.data & d2.data;
        public static int operator |(TestOperators1 d1, TestOperators1 d2) => d1.data | d2.data;
        public static int operator >>(TestOperators1 d1, int d2) => d1.data >> d2;
        public static int operator <<(TestOperators1 d1, int d2) => d1.data << d2;
        public static bool operator ==(TestOperators1 d1, TestOperators1 d2) => d1.data == d2.data;
        public static bool operator !=(TestOperators1 d1, TestOperators1 d2) => d1.data != d2.data;
        public static bool operator <(TestOperators1 d1, TestOperators1 d2) => d1.data < d2.data;
        public static bool operator <=(TestOperators1 d1, TestOperators1 d2) => d1.data <= d2.data;
        public static bool operator >(TestOperators1 d1, TestOperators1 d2) => d1.data > d2.data;
        public static bool operator >=(TestOperators1 d1, TestOperators1 d2) => d1.data >= d2.data;
    }

    public struct TestOperators2
    {
        public int data;
        public TestOperators2(int data_)
        {
            data = data_;
        }
        public static implicit operator TestOperators2(int value) => new TestOperators2(value);

        public static int operator +(TestOperators2 d1, int d2) => 55;
        public static int operator +(int d1, TestOperators2 d2) => 66;

        public static int operator ~(TestOperators2 d1) => ~d1.data;
        public static bool operator !(TestOperators2 d1) => true;
        public static int operator +(TestOperators2 d1, TestOperators2 d2) => d1.data + d2.data;
        public static int operator -(TestOperators2 d1, TestOperators2 d2) => d1.data - d2.data;
        public static int operator *(TestOperators2 d1, TestOperators2 d2) => d1.data * d2.data;
        public static int operator /(TestOperators2 d1, TestOperators2 d2) => d1.data / d2.data;
        public static int operator %(TestOperators2 d1, TestOperators2 d2) => d1.data % d2.data;
        public static int operator ^(TestOperators2 d1, TestOperators2 d2) => d1.data ^ d2.data;
        public static int operator &(TestOperators2 d1, TestOperators2 d2) => d1.data & d2.data;
        public static int operator |(TestOperators2 d1, TestOperators2 d2) => d1.data | d2.data;
        public static int operator >>(TestOperators2 d1, int d2) => d1.data >> d2;
        public static int operator <<(TestOperators2 d1, int d2) => d1.data << d2;
        public static bool operator ==(TestOperators2 d1, TestOperators2 d2) => d1.data == d2.data;
        public static bool operator !=(TestOperators2 d1, TestOperators2 d2) => d1.data != d2.data;
        public static bool operator <(TestOperators2 d1, TestOperators2 d2) => d1.data < d2.data;
        public static bool operator <=(TestOperators2 d1, TestOperators2 d2) => d1.data <= d2.data;
        public static bool operator >(TestOperators2 d1, TestOperators2 d2) => d1.data > d2.data;
        public static bool operator >=(TestOperators2 d1, TestOperators2 d2) => d1.data >= d2.data;
    }

    public struct TestOperators3
    {
        public int data;
        public TestOperators3(int data_)
        {
            data = data_;
        }

        // Note, in order to test that was used proper operator, we use fixed return here.

        public static implicit operator int(TestOperators3 value) => 777;

        public static int operator +(TestOperators3 d1, int d2) => 555;
        public static int operator +(int d1, TestOperators3 d2) => 666;
        public static int operator >>(TestOperators3 d1, int d2) => 777;
        public static int operator <<(TestOperators3 d1, int d2) => 888;
    }

    class Program
    {
        int int_i = 505;
        static test_nested test_nested_static_instance;

        static int stGetInt()
        {
            return 111;
        }

        static int stGetInt(int x)
        {
            return x * 2;
        }

        int getInt()
        {
            return 222;
        }

        static int TestTimeOut()
        {
            System.Threading.Thread.Sleep(10000);
            return 5;
        }

        static int TestArrayArg(int[] arrayArg, int i)
        {
            return arrayArg[0] + i;
        }

        static int TestArrayArg(test_array[] arrayT, int i)
        {
            return i;
        }

        static int TestArrayArg2(short[] arrayArg, int i)
        {
            return arrayArg[0] + i;
        }

        static int TestArray2Arg(int[,] multiArray2, int i)
        {
            return multiArray2[0,0] + i;
        }

        static int TestArray2Arg2(short[,] multiArray2, int i)
        {
            return multiArray2[0,0] + i;
        }

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
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK10");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK11");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK12");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK13");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK14");
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
            int i1 = 0;
            int i2 = 2;
            int i3 = 4;
            int i4 = 1;
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
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "array1[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "30", "int", "array1[i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "50", "int", "array1[i3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "array1[ 0]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "30", "int", "array1[2 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "50", "int", "array1 [ 4 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "TestArrayArg(array1, 5)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "TestArrayArg(array2, 11)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "TestArrayArg2(array1, 5)", "error:");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{int[2, 3]}", "int[,]", "multi_array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multi_array2[0,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multi_array2[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "104", "int", "multi_array2[1,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "102", "int", "multi_array2[0,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "104", "int", "multi_array2[i4,i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "102", "int", "multi_array2[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multi_array2[ 0 , 0 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multi_array2  [ 1,1 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "106", "int", "TestArray2Arg(multi_array2, 5)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "TestArray2Arg2(multi_array2, 5)", "error:");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array[4]}", "VSCodeTestEvaluate.test_array[]", "array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array}", "VSCodeTestEvaluate.test_array", "array2[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "201", "int", "array2[i1].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "201", "int", "array2[0].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "201", "int", "array2   [   0   ]   .   i"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.test_array}", "VSCodeTestEvaluate.test_array", "array2[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "array2[i2].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "array2[2].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "401", "int", "array2  [  2  ]  .  i"); // check spaces

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "this.int_i", "error:"); // error, Main is static method that don't have "this"
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "int_i", "error:"); // error, don't have "this" (no object of type "Program" was created)
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "not_declared", "error:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "array1[]", "error CS1519:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "multi_array2[]", "error CS1519:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "multi_array2[,]", "error CS1519:"); // error, no such variable

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test expression calculation.
            TestOperators1 testClass = new TestOperators1(12);
            TestOperators2 testStruct;
            TestOperators3 testStruct2;
            testStruct.data = 5;
            string testString1 = null;
            string testString2 = "test";
            int break_line2 = 1;                                                                    Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test", "static_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "float", "1u + 1f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "long", "1u + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "decimal", "1m + 1m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "decimal", "1m + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "decimal", "1 + 1m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "66", "int", "1 + testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "testClass + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "66", "int", "1 + testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "testStruct + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "666", "int", "1 + testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "testStruct2 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"stringC\"", "string", "\"string\" + 'C'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test\"", "string", "testString1 + testString2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test\"", "string", "testString2 + testString1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"\"", "string", "testString1 + testString1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"testtest\"", "string", "testString2 + testString2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test\"", "string", "\"\" + \"test\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test\"", "string", "\"test\" + \"\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"\"", "string", "\"\" + \"\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"testtest\"", "string", "\"test\" + \"test\"");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1UL + 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true + 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 + not_var", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1u + testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testClass + 1u", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1u + testStruct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct + 1u", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testClass + testStruct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct + testClass", "error CS0019");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "3 - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "decimal", "3m - 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true - 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 - not_var", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-11", "int", "1 - testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "testClass - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-4", "int", "1 - testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "testStruct - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-776", "int", "1 - testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "776", "int", "testStruct2 - 1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "2 * 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "decimal", "2m * 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true * 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 * not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "2 / 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "decimal", "2m / 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true / 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 / not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "2 % 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "decimal", "2m % 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true % 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 % not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "1 << 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m << 2m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true << 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 << not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "4 >> 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m >> 2m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true >> 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 >> not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-2", "int", "~1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-13", "int", "~testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-6", "int", "~testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "~1m", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "~true", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "~not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true && true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "true && false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false && true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false && false");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 && 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m && 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true && not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true || true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true || false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "false || true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false || false");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 || 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m || 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true || not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "true ^ true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true ^ false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "false ^ true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false ^ false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "1 ^ 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "1 ^ 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "0 ^ 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "0 ^ 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "14", "int", "testClass ^ 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "14", "int", "2 ^ testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "testClass ^ testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "testStruct ^ 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "2 ^ testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "testStruct ^ testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct2 ^ testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m ^ 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 ^ not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true & true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "true & false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false & true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false & false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "3 & 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "5 & 8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "10 & 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "0 & 7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "0 & 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "testClass & 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "2 & testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "12", "int", "testClass & testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "testStruct & 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "2 & testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "testStruct & testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct2 & testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m & 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 & not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true | true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true | false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "false | true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false | false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "13", "int", "5 | 8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "10 | 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "0 | 7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "0 | 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "14", "int", "testClass | 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "14", "int", "2 | testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "12", "int", "testClass | testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "testStruct | 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "2 | testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "testStruct | testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct2 | testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1m | 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 | not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "!true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "!testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "!testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "!1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "!1m", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "!not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "1 == 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "2 == 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "2m == 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 == not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1 != 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "2 != 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "2m != 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 != not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "1 < 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1 < 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1m < 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true < false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 < not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "2 > 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1 > 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1m > 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true > false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 > not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "1 <= 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "1 <= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1 <= 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "1m <= 0m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true <= false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 <= not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "2 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "1 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "0 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "0m >= 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "true >= false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1 >= not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "2 << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "888", "int", "testStruct2 << testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "24", "int", "testClass << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "testStruct << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "20", "int", "testStruct << 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1f << 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1f << 1f", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testClass << testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct << testStruct", "error CS0019");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "4 >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "777", "int", "testStruct2 >> testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "testClass >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "testStruct >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "testStruct >> 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1f >> 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1f >> 1f", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testClass >> testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "testStruct >> testStruct", "error CS0019");

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
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "cl.st", "error:"); // error CS0176: Member 'test_static_class1_t.st' cannot be accessed with an instance reference; qualify it with a type name instead
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "cl.cl", "error:"); // error CS0176: Member 'test_static_class1_t.cl' cannot be accessed with an instance reference; qualify it with a type name instead
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
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.not_declared", "error:"); // error, no such field in class
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_struct1_t.not_declared", "error:"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.st.not_declared", "error:"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_class1_t.cl.not_declared", "error:"); // error, no such field in class

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
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "Program.test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "VSCodeTestEvaluate.Program.test_nested_static_instance.nested_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.test_static_child", "error:");

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

                Label.Checkpoint("lambda_test2", "internal_var_test", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK9");
                    Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK9");

                    Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "mainVar", "error:");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"localVar\"", "string", "localVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"argVar\"", "string", "argVar");

                    Context.AddExceptionBreakpointFilterAllWithOptions("System.Exception");
                    Context.SetExceptionBreakpoints(@"__FILE__:__LINE__");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                int break_line6 = 1;                                                                     Label.Breakpoint("BREAK9");
            };

            lambda2("argVar");

            // Test internal "$exception" variable name.

            try
            {
                throw new System.Exception();                                                            Label.Breakpoint("BP_EXCEPTION");
            }
            catch {}

            Label.Checkpoint("internal_var_test", "literals_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "BP_EXCEPTION", "CLR", "always", "System.Exception");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BP_EXCEPTION");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Exception}", "System.Exception", "$exception");
            });

            // Test literals.

            Label.Checkpoint("literals_test", "conditional_access_test", (Object context) => {
                Context Context = (Context)context;
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BP_EXCEPTION");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"test\"", "string", "\"test\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"$exception\"", "string", "\"$exception\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "99 'c'", "char", "'c'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "decimal", "10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "double", "10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "float", "10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "15", "int", "0x0F");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "42", "int", "0b00101010");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "uint", "7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "long", "7L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "ulong", "7UL");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "true", "bool", "true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "false", "bool", "false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "object", "null");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "0b_0010_1010", "error"); // Error could be CS1013 or CS8107 here.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "'𐌞'", "error CS1012"); // '𐌞' character need 2 whcars and not supported

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test conditional access.

            test_child child_null;
            conditional_access2 conditional_access_null;
            conditional_access2 conditional_access = new conditional_access2();
            conditional_access3 conditional_access_double_null = new conditional_access3();
            conditional_access3 conditional_access_double = new conditional_access3();
            conditional_access_double.member = new conditional_access1();
            int[] array1_null;
            conditional_access2[] conditional_array_null;
            conditional_access2[] conditional_array = new conditional_access2[] { new conditional_access2(), new conditional_access2() };
            object_with_array object_with_array_null;
            object_with_array object_with_array = new object_with_array();
            int break_line10 = 1;                                                                        Label.Breakpoint("BREAK10");

            Label.Checkpoint("conditional_access_test", "method_calls_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK10");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK10");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.test_child", "child_null?.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.test_child", "child_null?.i_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "402", "int", "child?.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "302", "int", "child?.i_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.conditional_access1}", "VSCodeTestEvaluate.conditional_access1", "conditional_access?.member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "conditional_access?.member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access2", "conditional_access_null?.member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access2", "conditional_access_null?.member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access1", "conditional_access_double_null?.member?.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "conditional_access_double?.member?.i");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "array1?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "20", "int", "array1?[conditional_access?.member.index]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "int[]", "array1_null?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access2[]", "conditional_array_null?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access2[]", "conditional_array_null?[0].member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.conditional_access2[]", "conditional_array_null?[0].member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.conditional_access2}", "VSCodeTestEvaluate.conditional_access2", "conditional_array?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.conditional_access1}", "VSCodeTestEvaluate.conditional_access1", "conditional_array?[0].member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "conditional_array?[0].member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.object_with_array", "object_with_array_null?.array[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "552", "int", "object_with_array?.array[1]");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test method calls.

            int Calc3()
            {
                return 8;
            }

            MethodCallTest1 MethodCallTest = new MethodCallTest1();
            MethodCallTest1 methodCallTest1Null;
            MethodCallTest2 methodCallTest2 = new MethodCallTest2();
            MethodCallTest2 methodCallTest2Null;
            test_child TestCallChild = new test_child();
            test_parent TestCallParentOverride = new test_child();
            test_parent TestCallParent = new test_parent();

            decimal decimalToString = 1.01M;
            double doubleToString = 2.02;
            float floatToString = 3.03f;
            char charToString = 'c';
            bool boolToString = true;
            sbyte sbyteToString = -5;
            byte byteToString = 5;
            short shortToString = -6;
            ushort ushortToString = 6;
            int intToString = -7;
            uint uintToString = 7;
            long longToString = -8;
            ulong ulongToString = 8;
            string stringToString = "string";

            MethodCallTest3 mcTest3 = new MethodCallTest3();

            int break_line11 = 1;                                                                        Label.Breakpoint("BREAK11");

            Label.Checkpoint("method_calls_test", "unary_operators_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK11");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK11");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "MethodCallTest.Calc1()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "MethodCallTest.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "MethodCallTest?.Calc1()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "MethodCallTest?.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"VSCodeTestEvaluate.MethodCallTest1\"", "string", "MethodCallTest?.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.MethodCallTest1", "methodCallTest1Null?.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.NullReferenceException}", "System.NullReferenceException", "methodCallTest1Null.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.MethodCallTest2", "methodCallTest2Null?.member.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "methodCallTest2Null.member.Calc2()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.NullReferenceException}", "System.NullReferenceException", "methodCallTest2.nullMember.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.MethodCallTest1", "methodCallTest2.nullMember?.Calc2()");

                // Call non static method in static member.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "VSCodeTestEvaluate.MethodCallTest2.member1.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "MethodCallTest2.member1.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.MethodCallTest2.member1.Calc1()", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "MethodCallTest2.member1.Calc1()", "error:");

                // Call static method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "VSCodeTestEvaluate.MethodCallTest1.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "VSCodeTestEvaluate.MethodCallTest1.Calc2()", "error:");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "MethodCallTest1.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "MethodCallTest1.Calc2()", "error:");

                // Call virtual/override.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "TestCallChild.GetNumber()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "TestCallParentOverride.GetNumber()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10", "int", "TestCallParent.GetNumber()");

                // Call built-in types methods.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"1.01\"", "\"1,01\"", "string", "1.01M.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"1.01\"", "\"1,01\"", "string", "decimalToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"2.02\"", "\"2,02\"", "string", "2.02.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"2.02\"", "\"2,02\"", "string", "doubleToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"3.03\"", "\"3,03\"", "string", "3.03f.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"3.03\"", "\"3,03\"", "string", "floatToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"c\"", "string", "'c'.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"c\"", "string", "charToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"True\"", "string", "boolToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"-5\"", "string", "sbyteToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"5\"", "string", "byteToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"-6\"", "string", "shortToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"6\"", "string", "ushortToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"6\"", "string", "MethodCallTest?.Calc2().ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"7\"", "string", "7.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"-7\"", "string", "intToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"7\"", "string", "uintToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"-8\"", "string", "longToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"8\"", "string", "ulongToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"string\"", "string", "\"string\".ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"string\"", "string", "stringToString.ToString()");

                // Call with arguments.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2.0", "decimal", "mcTest3.Calc1(1.0M)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "123", "int", "mcTest3.Calc2(1, 2, 3)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "456", "int", "mcTest3.Calc2(4, 5, 6.0f)");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test unary plus and negation operators.

            char charUnary = '영';
            sbyte sbyteUnary = -5;
            byte byteUnary = 5;
            short shortUnary = -6;
            ushort ushortUnary = 6;
            int intUnary = -7;
            uint uintUnary = 7;
            long longUnary = -8;
            ulong ulongUnary = 8;

            decimal decimalUnary = 1.01M;
            double doubleUnary = 2.02;
            float floatUnary = 3.03f;

            enumUnary_t enumUnary = enumUnary_t.TWO;

            int break_line12 = 1;                                                                        Label.Breakpoint("BREAK12");

            Label.Checkpoint("unary_operators_test", "function_evaluation_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK12");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK12");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TWO", "VSCodeTestEvaluate.enumUnary_t", "enumUnary");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "+enumUnary", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "-enumUnary", "error CS0023");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "49", "int", "+'1'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-49", "int", "-'1'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "50689", "int", "+charUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-50689", "int", "-charUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-5", "int", "+sbyteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "-sbyteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "5", "int", "+byteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-5", "int", "-byteUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-6", "int", "+shortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "-shortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "+ushortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-6", "int", "-ushortUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "+7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-7", "int", "+intUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-7", "int", "-7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "-(-7)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-7", "int", "+(-7)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "int", "-intUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "uint", "+7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "7", "uint", "+uintUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-7", "long", "-7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-7", "long", "-uintUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "8", "long", "+8L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-8", "long", "+longUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-8", "long", "-8L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "8", "long", "-longUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "8", "ulong", "+8UL");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "8", "ulong", "+ulongUnary");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "-8UL", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "-ulongUnary", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "decimal", "+10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1.01", "decimal", "+decimalUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-10.5", "decimal", "-10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-1.01", "decimal", "-decimalUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "double", "+10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2.02", "double", "+doubleUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-10.5", "double", "-10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-2.02", "double", "-doubleUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "10.5", "float", "+10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "3.03", "float", "+floatUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-10.5", "float", "-10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "-3.03", "float", "-floatUnary");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int break_line13 = 13;                                                                        Label.Breakpoint("BREAK13");

            Label.Checkpoint("function_evaluation_test", "coalesce_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK13");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK13");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "stGetInt()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "222", "int", "stGetInt(111)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "getInt()", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "TestTimeOut()", "Evaluation timed out.");

                Context.Continue(@"__FILE__:__LINE__");
            });

            coalesce_test_A test_null = null;
            coalesce_test_A A_class = new coalesce_test_A();
            coalesce_test_B B_class = new coalesce_test_B();

            string str_null = null;
            string A_string = "A";
            string B_string = "B";
            string C_string = "C";

            test_struct1_t test_struct = new test_struct1_t();
            decimal test_decimal = 1;
            int test_int = 1;

            int break_line14 = 1;                                                                        Label.Breakpoint("BREAK14");
            Label.Checkpoint("coalesce_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK14");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK14");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"first\"", "string", "\"first\".ToString()??\"second\".ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.coalesce_test_A}", "VSCodeTestEvaluate.coalesce_test_A", "A_class??test_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.coalesce_test_A}", "VSCodeTestEvaluate.coalesce_test_A", "test_null??A_class");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.coalesce_test_A", "test_null??test_null");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.coalesce_test_A}", "VSCodeTestEvaluate.coalesce_test_A", "test_null??test_null??A_class");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.coalesce_test_A}", "VSCodeTestEvaluate.coalesce_test_A", "A_class??test_null??test_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{VSCodeTestEvaluate.coalesce_test_A}", "VSCodeTestEvaluate.coalesce_test_A", "test_null??A_class??test_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.coalesce_test_A", "test_null??test_null??test_null");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"B\"", "string", "str_null??B_string.ToString()??C_string.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"C\"", "string", "str_null??str_null??C_string.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"A\"", "string", "A_string.ToString()??str_null??C_string.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"A\"", "string", "A_string.ToString()??str_null??str_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"B\"", "string", "str_null??B_string.ToString()??str_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"A\"", "string", "A_string.ToString()??B_string.ToString()??str_null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"A\"", "string", "A_string.ToString()??B_string.ToString()??C_string.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvaluate.coalesce_test_A", "test_null??test_null??test_null");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_string.ToString()??1", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1??A_string.ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_class??1", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "1??A_class", "error CS0019");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_string.ToString()??test_int", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_int??A_string.ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_class??test_int", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_int??A_class", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_int??test_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_null??test_int", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_int??test_int", "error CS0019");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_string.ToString()??test_struct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_struct??A_string.ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_class??test_struct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_struct??A_class", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_null??test_struct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_struct??test_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_struct??test_struct", "error CS0019");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_string.ToString()??test_decimal", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_decimal??A_string.ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_class??test_decimal", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_decimal??A_class", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_null??test_decimal", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_decimal??test_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_decimal??test_decimal", "error CS0019");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "\"first\".ToString()??test_decimal", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_decimal??\"first\".ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "\"first\".ToString()??test_int", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_int??\"first\".ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "\"first\".ToString()??test_struct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_struct??\"first\".ToString()", "error CS0019");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_null??\"first\".ToString()", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "\"first\".ToString()??test_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "test_null??str_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "A_class??B_class", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "B_class??A_class", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "B_class??str_null", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "str_null??B_class", "error CS0019");

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
