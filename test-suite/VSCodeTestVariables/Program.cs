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

        public void CheckVariablesCount(string caller_trace, Int64 frameId, string ScopeName, int VarCount)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    Assert.True(VarCount == Scope.namedVariables
                                || (VarCount == 0 && null == Scope.namedVariables),
                                @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
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
                    Assert.Equal(Type, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(Value, Variable.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public VSCodeResult SetVariable(string caller_trace, int variablesReference, string Name, string Value)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            return VSCodeDebugger.Request(setVariableRequest);
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
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp3");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_func");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_getter");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            int i = 2;
            string test_string = "test";
            Console.WriteLine("i = " + i.ToString());
            Console.WriteLine(test_string);
            Console.WriteLine("A breakpoint \"bp\" is set on this line"); Label.Breakpoint("bp");

            Label.Checkpoint("bp_test", "bp_func_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp");
                Context.CheckVariablesCount(@"__FILE__:__LINE__", frameId, "Locals", 7);
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string[]", "args" , "{string[0]}");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "i", "2");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "test_string", "\"test\"");

                Assert.True(Context.SetVariable(@"__FILE__:__LINE__", variablesReference, "i", "5").Success, @"__FILE__:__LINE__");
                Assert.False(Context.SetVariable(@"__FILE__:__LINE__", variablesReference, "i", "\"string\"").Success, @"__FILE__:__LINE__");
                Assert.True(Context.SetVariable(@"__FILE__:__LINE__", variablesReference, "test_string", "\"changed_String\"").Success, @"__FILE__:__LINE__");
                Assert.False(Context.SetVariable(@"__FILE__:__LINE__", variablesReference, "test_string", "5").Success, @"__FILE__:__LINE__");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "i", "5");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "test_string", "\"changed_String\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestFunction(10);

            TestStruct4 ts4 = new TestStruct4();

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

        static void TestFunction(int t)
        {
            int f = 5;
            Console.WriteLine("f = " + f.ToString());                     Label.Breakpoint("bp_func");

            Label.Checkpoint("bp_func_test", "test_debugger_browsable_state", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_func");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_func");
                Context.CheckVariablesCount(@"__FILE__:__LINE__", frameId, "Locals", 2);
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "t", "10");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "int", "f", "5");
                Context.Continue(@"__FILE__:__LINE__");
            });
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
    }
}
