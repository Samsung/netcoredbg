using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

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

        public void GetAndCheckValue(string caller_trace, Int64 frameId, string Expression, string ExpectedResult)
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

            Assert.Equal(ExpectedResult, fixedVal, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void EvalVariableByIndex(string caller_trace, int variablesReference, string Type, int Index, string Value)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            if (Index < variablesResponse.body.variables.Count) {
                var Variable = variablesResponse.body.variables[Index];
                Assert.Equal(Type, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                Assert.Equal(Value, Variable.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
                return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void ErrorSetVariable(string caller_trace, int variablesReference, string Name, string Value)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            Assert.False(VSCodeDebugger.Request(setVariableRequest).Success, @"__FILE__:__LINE__");
        }

        public void SetExpression(string caller_trace, Int64 frameId, string Expression, string Value)
        {
            SetExpressionRequest setExpressionRequest = new SetExpressionRequest();
            setExpressionRequest.arguments.expression = Expression;
            setExpressionRequest.arguments.value = Value;
            Assert.True(VSCodeDebugger.Request(setExpressionRequest).Success, @"__FILE__:__LINE__");
        }

        public void ErrorSetExpression(string caller_trace, Int64 frameId, string Expression, string Value)
        {
            SetExpressionRequest setExpressionRequest = new SetExpressionRequest();
            setExpressionRequest.arguments.expression = Expression;
            setExpressionRequest.arguments.value = Value;
            Assert.False(VSCodeDebugger.Request(setExpressionRequest).Success, @"__FILE__:__LINE__");
        }

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        // NOTE this code works only with one source file
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestVariables
{
    public class TestImplicitCast1
    {
        public int data;
        public TestImplicitCast1(int data_)
        {
            data = data_;
        }

        public static implicit operator TestImplicitCast1(char value) => new TestImplicitCast1((int)value);
        public static implicit operator TestImplicitCast1(int value) => new TestImplicitCast1(value);
        public static implicit operator TestImplicitCast1(decimal value) => new TestImplicitCast1((int)value);
        public static implicit operator int(TestImplicitCast1 value) => value.data;
        public static implicit operator decimal(TestImplicitCast1 value) => (decimal)value.data;

        public override string ToString()
        {
            return data.ToString();
        }

        public decimal GetDecimal()
        {
            return 11.1M;
        }
    }

    public class TestImplicitCast2
    {
        private long data;
        public TestImplicitCast2(long data_)
        {
            data = data_;
        }

        public static implicit operator TestImplicitCast2(TestImplicitCast1 value) => new TestImplicitCast2(value.data * 10);

        public override string ToString()
        {
            return data.ToString();
        }
    }

    public struct TestImplicitCast3
    {
        private float data;

        public TestImplicitCast3(decimal data_)
        {
            data = (float)data_;
        }

        public static implicit operator TestImplicitCast3(decimal value) => new TestImplicitCast3(value);

        public override string ToString()
        {
            return data.ToString();
        }
    }

    public struct TestSetVarStruct
    {
        public static int static_field_i;
        public int field_i;

        public static int static_prop_i
        { get; set; }
        public int prop_i
        { get; set; }

        public static int static_prop_i_noset
        { get {return 5001;} }
        public int prop_i_noset
        { get {return 5002;} }
    }

    public struct TestSetExprStruct
    {
        public static int static_field_i;
        public int field_i;

        public static int static_prop_i
        { get; set; }
        public int prop_i
        { get; set; }

        public static int static_prop_i_noset
        { get {return 5001;} }
        public int prop_i_noset
        { get {return 5002;} }
    }

    public struct TestStruct4
    {
        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
        public int val1
        {
            get
            {
                return 666; 
            }
        }

        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.Never)]
        public int val2
        {
            get
            {
                return 777; 
            }
        }

        public int val3
        {
            get
            {
                return 888; 
            }
        }
    }

    public struct TestStruct5
    {
        public int val1
        {
            get
            {
                return 111; 
            }
        }

        public int val2
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 222; 
            }
        }

        public string val3
        {
            get
            {
                return "text_333"; 
            }
        }

        public float val4
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 444.4f; 
            }
        }

        public float val5
        {
            get
            {
                return 555.5f; 
            }
        }
    }

    public struct TestStruct6
    {
        public int val1
        {
            get
            {
                // Test, that debugger ignore Break() callback during eval.
                Debugger.Break();
                return 123; 
            }
        }

        public int val2
        {
            get
            {
                System.Threading.Thread.Sleep(5000000);
                return 999; 
            }
        }

        public string val3
        {
            get
            {
                // Test, that debugger ignore Breakpoint() callback during eval.
                return "text_123";                              Label.Breakpoint("bp_getter");
            }
        }
    }

    public struct TestStruct7
    {
        public int val1
        {
            get
            {
                return 567; 
            }
        }

        public int val2
        {
            get
            {
                try {
                    throw new System.DivideByZeroException();
                }
                catch
                {
                    return 777; 
                }
                return 888; 
            }
        }

        public int val3
        {
            get
            {
                throw new System.DivideByZeroException();
                return 777; 
            }
        }

        public string val4
        {
            get
            {
                return "text_567"; 
            }
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "setup_var", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp3");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_func1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_func2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_getter");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test set variable.

            sbyte   testSByte = -2;
            byte    testByte = 1;
            short   testShort = -3;
            ushort  testUShort = 4;
            int     testInt = -5;
            uint    testUInt = 6;
            long    testLong = -7;
            ulong   testULong = 8;
            float   testFloat = 9.9f;
            double  testDouble = 10.1;
            decimal testDecimal = 11.11M;
            char    testChar = 'ㅎ';
            bool    testBool = true;
            string  testString = "some string that I'll test with";
            TestImplicitCast1 testClass = new TestImplicitCast1(12);

            sbyte   varSByte = -102;
            byte    varByte = 101;
            short   varShort = -103;
            ushort  varUShort = 104;
            int     varInt = -105;
            uint    varUInt = 106;
            long    varLong = -107;
            ulong   varULong = 108;
            float   varFloat = 109.9f;
            double  varDouble = 1010.1;
            decimal varDecimal = 1011.11M;
            char    varChar = 'Ф';
            bool    varBool = false;
            string  varString = "another string";
            TestImplicitCast1 varClass = new TestImplicitCast1(112);
            TestImplicitCast2 varClass2 = new TestImplicitCast2(312);
            TestImplicitCast3 varStruct3;

            sbyte   litSByte = -103;
            byte    litByte = 102;
            short   litShort = -104;
            ushort  litUShort = 204;
            int     litInt = -205;
            uint    litUInt = 206;
            long    litLong = -207;
            ulong   litULong = 208;
            float   litFloat = 209.9f;
            double  litDouble = 2010.1;
            decimal litDecimal = 2011.11M;
            char    litChar = 'Й';
            bool    litBool = false;
            string  litString = "string";
            TestImplicitCast1 litClass = new TestImplicitCast1(212);

            int[] array1 = new int[] { 1, 2, 3, 4, 5 };

            TestSetVarStruct setVarStruct = new TestSetVarStruct();
            TestSetVarStruct.static_field_i = 1001;
            TestSetVarStruct.static_prop_i = 1002;
            setVarStruct.field_i = 2001;
            setVarStruct.prop_i = 2002;

            TestSetExprStruct setExprStruct = new TestSetExprStruct();
            TestSetExprStruct.static_field_i = 1001;
            TestSetExprStruct.static_prop_i = 1002;
            setExprStruct.field_i = 2001;
            setExprStruct.prop_i = 2002;

            int dummy1 = 1;                                     Label.Breakpoint("BREAK1");

            Label.Checkpoint("setup_var", "test_var", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varChar", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testSByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varChar", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litChar", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310.1f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310.1d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "310.1m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litChar", "\"string\"");

                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varByte", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varByte", "testClass");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "301");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litByte", "103");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "-103");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "103u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "-103L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "103ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "103f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "103d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "103m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litByte", "\"string\"");

                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varSByte", "testSByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varSByte", "testClass");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "-301");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litSByte", "-105");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "103u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "-103L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "103ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "-103f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "-103d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "-103m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litSByte", "\"string\"");

                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varShort", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varShort", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varShort", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varShort", "testClass");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "-30000005");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litShort", "-205");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "205u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "-205L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "205ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "205f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "205d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "205m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litShort", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUShort", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUShort", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUShort", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUShort", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litUShort", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "30000005");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litUShort", "205");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "-205");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "205m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUShort", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varInt", "testString");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varInt", "testClass", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litInt", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "-2147483649");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litInt", "-305");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "305u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "-305L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "305ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "-305f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "-305d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "-305m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litInt", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUInt", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUInt", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUInt", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varUInt", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varUInt", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litUInt", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "4294967297");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litUInt", "306");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "-306");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litUInt", "306u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "306L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "306ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "306f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "306d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "306m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litUInt", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testUInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varLong", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varLong", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litLong", "'A'");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litLong", "-307");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litLong", "307u");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litLong", "-307L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "307ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "-307f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "-307d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "-307m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litLong", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varULong", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varULong", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varULong", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varULong", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testLong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varULong", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varULong", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litULong", "'A'");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litULong", "308");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "-308");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litULong", "308u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "308L");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litULong", "308ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "308f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "308d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "308m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litULong", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testUInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testLong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testULong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varFloat", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varFloat", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varFloat", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varFloat", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varFloat", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varFloat", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "'A'");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "309");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "309u");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "309L");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "309ul");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litFloat", "309.9f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litFloat", "309.9d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litFloat", "309.9m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litFloat", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litFloat", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testUInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testLong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testULong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testFloat", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDouble", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDouble", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDouble", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDouble", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDouble", "testClass");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "'A'");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310u");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310L");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310ul");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310.1f", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDouble", "310.1d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDouble", "310.1m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDouble", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDouble", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testChar");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testSByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testByte");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testUInt");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testLong");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDecimal", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDecimal", "testDouble");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDecimal", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varDecimal", "testString");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varDecimal", "testClass", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "'A'");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "311");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "311u");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "311L");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "311ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDecimal", "311.11f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDecimal", "311.11d");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litDecimal", "311.11m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDecimal", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litDecimal", "\"string\"");

                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testSByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testDecimal");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varBool", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varBool", "testClass");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310.1f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310.1d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "310.1m");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litBool", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litBool", "\"string\"");

                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testChar");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testSByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testUShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testDouble");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testDecimal");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testBool");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varString", "testString");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varString", "testClass");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "'A'");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310.1f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310.1d");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "310.1m");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litString", "true");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litString", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass", "testChar", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"12622\"");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testSByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testByte");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testShort");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testUShort");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass", "testInt", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"-5\"");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testUInt");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testLong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testULong");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testFloat");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testDouble");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass", "testDecimal", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"11\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass", "varClass.GetDecimal()", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"11\"");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testBool");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "varClass", "testString");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass", "testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"12\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litClass", "'A'", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litClass.ToString()", "\"65\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litClass", "5", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litClass.ToString()", "\"5\"");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "310u");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "310L");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "310ul");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "310.1f");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "310.1d");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "litClass", "310.1m", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litClass.ToString()", "\"310\"");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "true");
                Context.ErrorSetVariable(@"__FILE__:__LINE__", variablesReference, "litClass", "\"string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varClass2", "testClass", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass2.ToString()", "\"120\"");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "varStruct3", "11m", true);
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varStruct3.ToString()", "\"11\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[0]", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[1]", "2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[2]", "3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[3]", "4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[4]", "5");
                int array1Reference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference, "array1");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, array1Reference, "[1]", "11");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, array1Reference, "[3]", "33");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_field_i", "1001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_prop_i", "1002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_prop_i_noset", "5001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.field_i", "2001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.prop_i", "2002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.prop_i_noset", "5002");
                int setVarStructReference = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference, "setVarStruct");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setVarStructReference, "static_field_i", "3001", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setVarStructReference, "static_prop_i", "3002", true);
                Context.ErrorSetVariable(@"__FILE__:__LINE__", setVarStructReference, "static_prop_i_noset", "3003");
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setVarStructReference, "field_i", "4001", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, setVarStructReference, "prop_i", "4002", true);
                Context.ErrorSetVariable(@"__FILE__:__LINE__", setVarStructReference, "prop_i_noset", "4003");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_field_i", "3001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_prop_i", "3002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.field_i", "4001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.prop_i", "4002");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_field_i", "1001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i", "1002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i_noset", "5001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.field_i", "2001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i", "2002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i_noset", "5002");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_field_i", "3001");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i", "3002");
                Context.ErrorSetExpression(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i_noset", "3003");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "setExprStruct.field_i", "4001");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i", "4002");
                Context.ErrorSetExpression(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i_noset", "4003");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_field_i", "3001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i", "3002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.field_i", "4001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i", "4002");
                Context.ErrorSetExpression(@"__FILE__:__LINE__", frameId, "1+1", "2");
                Context.ErrorSetExpression(@"__FILE__:__LINE__", frameId, "1", "1");
                Context.ErrorSetExpression(@"__FILE__:__LINE__", frameId, "1.ToString()", "\"1\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                     Label.Breakpoint("BREAK2");

            Label.Checkpoint("test_var", "bp_func_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK2");
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varChar", "12622");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litChar", "65");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varSByte", "-2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litSByte", "-105");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varByte", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litByte", "103");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varShort", "-3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litShort", "-205");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varUShort", "4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litUShort", "205");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varInt", "12");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litInt", "-305");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varUInt", "6");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litUInt", "306");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varLong", "-7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litLong", "-307");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varULong", "8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litULong", "308");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varFloat", "9.8999996");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litFloat", "309.89999");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varDouble", "10.1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litDouble", "310.1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varDecimal", "12");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litDecimal", "311.11");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varBool", "true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litBool", "true");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varString", "\"some string that I'll test with\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litString", "\"string\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass.ToString()", "\"12\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "litClass.ToString()", "\"310\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varClass2.ToString()", "\"120\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "varStruct3.ToString()", "\"11\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[0]", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[1]", "11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[2]", "3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[3]", "33");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "array1[4]", "5");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_field_i", "3001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetVarStruct.static_prop_i", "3002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.field_i", "4001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setVarStruct.prop_i", "4002");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_field_i", "3001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "TestSetExprStruct.static_prop_i", "3002");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.field_i", "4001");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "setExprStruct.prop_i", "4002");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestFunctionArgs(10, 5f, "test_string");

            TestStruct4 ts4 = new TestStruct4();

            int i = 0;
            i++;                                                           Label.Breakpoint("bp2");

            Label.Checkpoint("test_debugger_browsable_state", "test_NotifyOfCrossThreadDependency", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp2");

                int variablesReference_Locals = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int variablesReference_ts4 = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference_Locals, "ts4");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts4, "int", "val1", "666");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts4, "int", "val3", "888");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts4, "int", 0, "666");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts4, "int", 1, "888");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct5 ts5 = new TestStruct5();

            // part of NotifyOfCrossThreadDependency test, no active evaluation here for sure
            System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();

            i++;                                                            Label.Breakpoint("bp3");

            Label.Checkpoint("test_NotifyOfCrossThreadDependency", "test_eval_timeout", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp3");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp3");

                int variablesReference_Locals = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int variablesReference_ts5 = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference_Locals, "ts5");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts5, "int", "val1", "111");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts5, "", "val2", "<error>");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts5, "string", "val3", "\"text_333\"");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts5, "", "val4", "<error>");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts5, "float", "val5", "555.5");

                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts5, "int", 0, "111");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts5, "", 1, "<error>");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts5, "string", 2, "\"text_333\"");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts5, "", 3, "<error>");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts5, "float", 4, "555.5");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct6 ts6 = new TestStruct6();

            i++;                                                            Label.Breakpoint("bp4");

            Label.Checkpoint("test_eval_timeout", "test_eval_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp4");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp4");

                int variablesReference_Locals = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int variablesReference_ts6 = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference_Locals, "ts6");

                var task = System.Threading.Tasks.Task.Run(() => 
                {
                    Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts6, "int", "val1", "123");
                    Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts6, "", "val2", "<error>");
                    Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts6, "string", "val3", "\"text_123\"");
                });
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve)
                if (!task.Wait(TimeSpan.FromSeconds(20)))
                    throw new DebuggerTimedOut(@"__FILE__:__LINE__");

                task = System.Threading.Tasks.Task.Run(() => 
                {
                    Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts6, "int", 0, "123");
                    Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts6, "", 1, "<error>");
                    Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts6, "string", 2, "\"text_123\"");
                });
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve)
                if (!task.Wait(TimeSpan.FromSeconds(20)))
                    throw new DebuggerTimedOut(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct7 ts7 = new TestStruct7();

            i++;                                                            Label.Breakpoint("bp5");

            Label.Checkpoint("test_eval_exception", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp5");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp5");

                int variablesReference_Locals = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int variablesReference_ts7 = Context.GetChildVariablesReference(@"__FILE__:__LINE__", variablesReference_Locals, "ts7");

                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts7, "int", "val1", "567");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts7, "int", "val2", "777");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts7, "System.DivideByZeroException", "val3", "{System.DivideByZeroException}");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference_ts7, "string", "val4", "\"text_567\"");

                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts7, "int", 0, "567");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts7, "int", 1, "777");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts7, "System.DivideByZeroException", 2, "{System.DivideByZeroException}");
                Context.EvalVariableByIndex(@"__FILE__:__LINE__", variablesReference_ts7, "string", 3, "\"text_567\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void TestFunctionArgs(int test_arg_i, float test_arg_f, string test_arg_string)
        {
            int dummy1 = 1;                                     Label.Breakpoint("bp_func1");

            Label.Checkpoint("bp_func_test", "bp_func_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_func1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_func1");
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");

                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "test_arg_i", "10");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "float", "test_arg_f", "5");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "test_arg_string", "\"test_string\"");

                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "test_arg_i", "20", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "test_arg_f", "50", true);
                Context.SetVariable(@"__FILE__:__LINE__", frameId, variablesReference, "test_arg_string", "\"edited_string\"", true);

                Context.Continue(@"__FILE__:__LINE__");
            });

            dummy1 = 2;                                         Label.Breakpoint("bp_func2");

            Label.Checkpoint("bp_func_test2", "test_debugger_browsable_state", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_func2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_func2");
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");

                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "test_arg_i", "20");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "float", "test_arg_f", "50");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "test_arg_string", "\"edited_string\"");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
