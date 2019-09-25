using System;
using System.IO;
using System.Collections.Generic;

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
            // NOTE this code works only with one source file
            launchRequest.arguments.cwd = Directory.GetParent(DebuggeeInfo.SourceFilesPath).FullName;
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
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                        && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry"));

            foreach (var Event in VSCodeDebugger.EventList) {
                if (VSCodeDebugger.isResponseContainProperty(Event, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(Event, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(Event, "threadId"));
                    break;
                }
            }
        }

        public static void WasExit()
        {
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated"));
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
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                        && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint"));

            foreach (var Event in VSCodeDebugger.EventList) {
                if (VSCodeDebugger.isResponseContainProperty(Event, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(Event, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(Event, "threadId"));
                }
            }

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
                Context.CheckVariablesCount(frameId, "Locals", 3);
                int variablesReference = Context.GetVariablesReference(frameId, "Locals");
                Context.EvalVariable(variablesReference, "string[]", "arg" , "{string[0]}");
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

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static void TestFunction(int t)
        {
            int f = 5;
            Console.WriteLine("f = " + f.ToString());                     Label.Breakpoint("bp_func");

            Label.Checkpoint("bp_func_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp_func"]);
                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp_func"]);
                Context.CheckVariablesCount(frameId, "Locals", 2);
                int variablesReference = Context.GetVariablesReference(frameId, "Locals");
                Context.EvalVariable(variablesReference, "int", "t", "10");
                Context.EvalVariable(variablesReference, "int", "f", "5");
                Context.Continue();
            });
        }
    }
}
