using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Xunit;
using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public static void PrepareStart()
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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = DebuggeeInfo.TargetAssemblyPath;
            launchRequest.arguments.cwd = "";
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success);
        }

        public static void PrepareEnd()
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success);
        }

        public static void WasEntryPointHit()
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasExit()
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

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void DebuggerExit()
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success);
        }

        public static void AddBreakpoint(string bpName, string Condition = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition));
            BreakpointLines.Add(lbp.NumLine);
        }

        public static void SetBreakpoints()
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = BreakpointSourceName;
            // NOTE this code works only with one source file
            setBreakpointsRequest.arguments.source.path = DebuggeeInfo.SourceFilesPath;
            setBreakpointsRequest.arguments.lines.AddRange(BreakpointLines);
            setBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            setBreakpointsRequest.arguments.sourceModified = false;
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success);
        }

        public static void WasBreakpointHit(Breakpoint breakpoint)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success);

            Assert.Equal(BreakpointType.Line, breakpoint.Type);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == lbp.NumLine
                    && Frame.source.name == lbp.FileName
                    // NOTE this code works only with one source file
                    && Frame.source.path == DebuggeeInfo.SourceFilesPath)
                    return;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static Int64 DetectFrameId(Breakpoint breakpoint)
        {
            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success);

            Assert.Equal(BreakpointType.Line, breakpoint.Type);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == lbp.NumLine
                    && Frame.source.name == lbp.FileName
                    // NOTE this code works only with one source file
                    && Frame.source.path == DebuggeeInfo.SourceFilesPath)
                    return Frame.id;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void CheckVariablesCount(Int64 frameId, string ScopeName, int VarCount)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    Assert.True(VarCount == Scope.namedVariables
                                || (VarCount == 0 && null == Scope.namedVariables));
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static int GetVariablesReference(Int64 frameId, string ScopeName)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    return Scope.variablesReference == null ? 0 : (int)Scope.variablesReference;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static int GetChildVariablesReference(int VariablesReference, string VariableName)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = VariablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == VariableName)
                    return Variable.variablesReference;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void EvalVariable(int variablesReference, string Type, string Name, string Value)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == Name) {
                    Assert.True(Type == Variable.type
                                && Value == Variable.value);
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void EvalVariableByIndex(int variablesReference, string Type, int Index, string Value)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            if (Index < variablesResponse.body.variables.Count) {
                var Variable = variablesResponse.body.variables[Index];
                Assert.True(Type == Variable.type
                            && Value == Variable.value);
                return;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static VSCodeResult SetVariable(int variablesReference, string Name, string Value)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            return VSCodeDebugger.Request(setVariableRequest);
        }

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
        // NOTE this code works only with one source file
        public static string BreakpointSourceName;
        public static List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        public static List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestVariables
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test", () => {
                Context.PrepareStart();
                Context.AddBreakpoint("bp");
                Context.AddBreakpoint("bp2");
                Context.AddBreakpoint("bp3");
                Context.AddBreakpoint("bp4");
                Context.AddBreakpoint("bp5");
                Context.AddBreakpoint("bp_func");
                Context.SetBreakpoints();
                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.Continue();
            });

            int i = 2;
            string test_string = "test";
            Console.WriteLine("i = " + i.ToString());
            Console.WriteLine(test_string);
            Console.WriteLine("A breakpoint \"bp\" is set on this line"); Label.Breakpoint("bp");

            Label.Checkpoint("bp_test", "bp_func_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp"]);
                Context.CheckVariablesCount(frameId, "Locals", 7);
                int variablesReference = Context.GetVariablesReference(frameId, "Locals");
                Context.EvalVariable(variablesReference, "string[]", "args" , "{string[0]}");
                Context.EvalVariable(variablesReference, "int", "i", "2");
                Context.EvalVariable(variablesReference, "string", "test_string", "\"test\"");

                Assert.True(Context.SetVariable(variablesReference, "i", "5").Success);
                Assert.False(Context.SetVariable(variablesReference, "i", "\"string\"").Success);
                Assert.True(Context.SetVariable(variablesReference, "test_string", "\"changed_String\"").Success);
                Assert.False(Context.SetVariable(variablesReference, "test_string", "5").Success);
                Context.EvalVariable(variablesReference, "int", "i", "5");
                Context.EvalVariable(variablesReference, "string", "test_string", "\"changed_String\"");

                Context.Continue();
            });

            TestFunction(10);

            TestStruct4 ts4 = new TestStruct4();

            i++;                                                           Label.Breakpoint("bp2");

            Label.Checkpoint("test_debugger_browsable_state", "test_NotifyOfCrossThreadDependency", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp2"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp2"]);

                int variablesReference_Locals = Context.GetVariablesReference(frameId, "Locals");
                int variablesReference_ts4 = Context.GetChildVariablesReference(variablesReference_Locals, "ts4");
                Context.EvalVariable(variablesReference_ts4, "int", "val1", "666");
                Context.EvalVariable(variablesReference_ts4, "int", "val3", "888");
                Context.EvalVariableByIndex(variablesReference_ts4, "int", 0, "666");
                Context.EvalVariableByIndex(variablesReference_ts4, "int", 1, "888");

                Context.Continue();
            });

            TestStruct5 ts5 = new TestStruct5();

            // part of NotifyOfCrossThreadDependency test, no active evaluation here for sure
            System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();

            i++;                                                            Label.Breakpoint("bp3");

            Label.Checkpoint("test_NotifyOfCrossThreadDependency", "test_eval_timeout", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp3"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp3"]);

                int variablesReference_Locals = Context.GetVariablesReference(frameId, "Locals");
                int variablesReference_ts5 = Context.GetChildVariablesReference(variablesReference_Locals, "ts5");
                Context.EvalVariable(variablesReference_ts5, "int", "val1", "111");
                Context.EvalVariable(variablesReference_ts5, "", "val2", "<error>");
                Context.EvalVariable(variablesReference_ts5, "string", "val3", "\"text_333\"");
                Context.EvalVariable(variablesReference_ts5, "", "val4", "<error>");
                Context.EvalVariable(variablesReference_ts5, "float", "val5", "555.5");

                Context.EvalVariableByIndex(variablesReference_ts5, "int", 0, "111");
                Context.EvalVariableByIndex(variablesReference_ts5, "", 1, "<error>");
                Context.EvalVariableByIndex(variablesReference_ts5, "string", 2, "\"text_333\"");
                Context.EvalVariableByIndex(variablesReference_ts5, "", 3, "<error>");
                Context.EvalVariableByIndex(variablesReference_ts5, "float", 4, "555.5");

                Context.Continue();
            });

            TestStruct6 ts6 = new TestStruct6();

            i++;                                                            Label.Breakpoint("bp4");

            Label.Checkpoint("test_eval_timeout", "test_eval_exception", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp4"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp4"]);

                int variablesReference_Locals = Context.GetVariablesReference(frameId, "Locals");
                int variablesReference_ts6 = Context.GetChildVariablesReference(variablesReference_Locals, "ts6");

                var task = System.Threading.Tasks.Task.Run(() => 
                {
                    Context.EvalVariable(variablesReference_ts6, "int", "val1", "123");
                    Context.EvalVariable(variablesReference_ts6, "", "val2", "<error>");
                    Context.EvalVariable(variablesReference_ts6, "string", "val3", "\"text_123\"");
                });
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve)
                if (!task.Wait(TimeSpan.FromSeconds(20)))
                    throw new NetcoreDbgTestCore.DebuggerTimedOut();

                task = System.Threading.Tasks.Task.Run(() => 
                {
                    Context.EvalVariableByIndex(variablesReference_ts6, "int", 0, "123");
                    Context.EvalVariableByIndex(variablesReference_ts6, "", 1, "<error>");
                    Context.EvalVariableByIndex(variablesReference_ts6, "string", 2, "\"text_123\"");
                });
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve)
                if (!task.Wait(TimeSpan.FromSeconds(20)))
                    throw new NetcoreDbgTestCore.DebuggerTimedOut();

                Context.Continue();
            });

            TestStruct7 ts7 = new TestStruct7();

            i++;                                                            Label.Breakpoint("bp5");

            Label.Checkpoint("test_eval_exception", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp5"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp5"]);

                int variablesReference_Locals = Context.GetVariablesReference(frameId, "Locals");
                int variablesReference_ts7 = Context.GetChildVariablesReference(variablesReference_Locals, "ts7");

                Context.EvalVariable(variablesReference_ts7, "int", "val1", "567");
                Context.EvalVariable(variablesReference_ts7, "int", "val2", "777");
                Context.EvalVariable(variablesReference_ts7, "System.DivideByZeroException", "val3", "{System.DivideByZeroException}");
                Context.EvalVariable(variablesReference_ts7, "string", "val4", "\"text_567\"");

                Context.EvalVariableByIndex(variablesReference_ts7, "int", 0, "567");
                Context.EvalVariableByIndex(variablesReference_ts7, "int", 1, "777");
                Context.EvalVariableByIndex(variablesReference_ts7, "System.DivideByZeroException", 2, "{System.DivideByZeroException}");
                Context.EvalVariableByIndex(variablesReference_ts7, "string", 3, "\"text_567\"");

                Context.Continue();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static void TestFunction(int t)
        {
            int f = 5;
            Console.WriteLine("f = " + f.ToString());                     Label.Breakpoint("bp_func");

            Label.Checkpoint("bp_func_test", "test_debugger_browsable_state", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp_func"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp_func"]);
                Context.CheckVariablesCount(frameId, "Locals", 2);
                int variablesReference = Context.GetVariablesReference(frameId, "Locals");
                Context.EvalVariable(variablesReference, "int", "t", "10");
                Context.EvalVariable(variablesReference, "int", "f", "5");
                Context.Continue();
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
                    return "text_123"; 
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
