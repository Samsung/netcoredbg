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

        public void AddBreakpoint(string caller_trace, string bpName, string bpPath = null, string Condition = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;
            string sourceFile = bpPath != null ? bpPath : lbp.FileName;

            List<SourceBreakpoint> listBp;
            if (!SrcBreakpoints.TryGetValue(sourceFile, out listBp)) {
                listBp = new List<SourceBreakpoint>();
                SrcBreakpoints[sourceFile] = listBp;
            }
            listBp.Add(new SourceBreakpoint(lbp.NumLine, Condition));

            List<int?> listBpId;
            if (!SrcBreakpointIds.TryGetValue(sourceFile, out listBpId)) {
                listBpId = new List<int?>();
                SrcBreakpointIds[sourceFile] = listBpId;
            }
            listBpId.Add(null);
        }

        public void RemoveBreakpoint(string caller_trace, string bpName, string bpPath = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;
            string sourceFile = bpPath != null ? bpPath : lbp.FileName;

            List<SourceBreakpoint> listBp;
            Assert.True(SrcBreakpoints.TryGetValue(sourceFile, out listBp), @"__FILE__:__LINE__"+"\n"+caller_trace);

            List<int?> listBpId;
            Assert.True(SrcBreakpointIds.TryGetValue(sourceFile, out listBpId), @"__FILE__:__LINE__"+"\n"+caller_trace);

            int indexBp = listBp.FindIndex(x => x.line == lbp.NumLine);
            listBp.RemoveAt(indexBp);
            listBpId.RemoveAt(indexBp);
        }

        public int? GetBreakpointId(string caller_trace, string bpName, string bpPath = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;
            string sourceFile = bpPath != null ? bpPath : lbp.FileName;

            List<SourceBreakpoint> listBp;
            Assert.True(SrcBreakpoints.TryGetValue(sourceFile, out listBp), @"__FILE__:__LINE__"+"\n"+caller_trace);

            List<int?> listBpId;
            Assert.True(SrcBreakpointIds.TryGetValue(sourceFile, out listBpId), @"__FILE__:__LINE__"+"\n"+caller_trace);

            int indexBp = listBp.FindIndex(x => x.line == lbp.NumLine);
            return listBpId[indexBp];
        }

        public void SetBreakpoints(string caller_trace)
        {
            foreach (var Breakpoints in SrcBreakpoints) {
                SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
                setBreakpointsRequest.arguments.source.name = Path.GetFileName(Breakpoints.Key);

                setBreakpointsRequest.arguments.source.path = Breakpoints.Key;
                setBreakpointsRequest.arguments.breakpoints.AddRange(Breakpoints.Value);
                setBreakpointsRequest.arguments.sourceModified = false;
                var ret = VSCodeDebugger.Request(setBreakpointsRequest);
                Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

                SetBreakpointsResponse setBreakpointsResponse =
                    JsonConvert.DeserializeObject<SetBreakpointsResponse>(ret.ResponseStr);

                // check, that we don't have hiddenly re-created breakpoints with different ids
                for (int i = 0; i < setBreakpointsResponse.body.breakpoints.Count; i++) {
                    if (SrcBreakpointIds[Breakpoints.Key][i] == null) {
                        CurrentBpId++;
                        SrcBreakpointIds[Breakpoints.Key][i] = setBreakpointsResponse.body.breakpoints[i].id;
                    } else {
                        Assert.Equal(SrcBreakpointIds[Breakpoints.Key][i], setBreakpointsResponse.body.breakpoints[i].id, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    }
                }
            }
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

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == lbp.NumLine
                    && Frame.source.name == lbp.FileName)
                    return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddManualBreakpoint(string caller_trace, string bp_fileName, int bp_line)
        {
            List<SourceBreakpoint> listBp;
            if (!SrcBreakpoints.TryGetValue(bp_fileName, out listBp)) {
                listBp = new List<SourceBreakpoint>();
                SrcBreakpoints[bp_fileName] = listBp;
            }
            listBp.Add(new SourceBreakpoint(bp_line, null));

            List<int?> listBpId;
            if (!SrcBreakpointIds.TryGetValue(bp_fileName, out listBpId)) {
                listBpId = new List<int?>();
                SrcBreakpointIds[bp_fileName] = listBpId;
            }
            listBpId.Add(null);
        }

        public void WasManualBreakpointHit(string caller_trace, string bp_fileName, int bp_line)
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

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == bp_line
                    && Frame.source.name == bp_fileName)
                    return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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
        public int CurrentBpId = 0;
        // Note, SrcBreakpoints and SrcBreakpointIds must have same order of the elements, since we use indexes for mapping.
        Dictionary<string, List<SourceBreakpoint>> SrcBreakpoints = new Dictionary<string, List<SourceBreakpoint>>();
        Dictionary<string, List<int?>> SrcBreakpointIds = new Dictionary<string, List<int?>>();
    }
}

namespace VSCodeTestSrcBreakpointResolve
{
    class test_constructors
    {
        int test_field = 5; // bp here! make sure you correct code (test constructor)!

        public test_constructors()
        {

        }

        public test_constructors(int i)
        {
            
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                // setup breakpoints before process start
                // in this way we will check breakpoint resolve routine during module load

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp2", "../Program.cs");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp3", "VSCodeTestSrcBreakpointResolve/Program.cs");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp4", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test2");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp0_delete_test1");
Label.Breakpoint("bp0_delete_test2");
Label.Breakpoint("bp1");
Label.Breakpoint("bp2");
Label.Breakpoint("bp3");
Label.Breakpoint("resolved_bp1");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp4");

            Label.Checkpoint("bp_test1", "bp_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp1");

                // check, that we have proper breakpoint ids
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1"); // previously was deleted with id1
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                int? id7 = Context.GetBreakpointId(@"__FILE__:__LINE__", "bp0_delete_test1");
                Assert.Equal(Context.CurrentBpId, id7, @"__FILE__:__LINE__");
                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                int? id_bp5_b = Context.GetBreakpointId(@"__FILE__:__LINE__", "bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Assert.Equal(Context.CurrentBpId, id_bp5_b, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp5_resolve_wrong_source"); // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
                                        Console.WriteLine("Hello World!");

Label.Breakpoint("bp5");                // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
Label.Breakpoint("resolved_bp2");       Console.WriteLine("Hello World!");

            Label.Checkpoint("bp_test2", "bp_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp2");

                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp5");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                bool isWindows = System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows);
                if (isWindows)
                    Context.AddBreakpoint(@"__FILE__:__LINE__", "bp6", "./VSCodeTestSrcBreakpointResolve/PROGRAM.CS");
                else
                    Context.AddBreakpoint(@"__FILE__:__LINE__", "bp6", "./VSCodeTestSrcBreakpointResolve/Program.cs");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                int? id_bp6_b = Context.GetBreakpointId(@"__FILE__:__LINE__", "bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Assert.Equal(Context.CurrentBpId, id_bp6_b, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

                                        Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6_resolve_wrong_source");
Label.Breakpoint("resolved_bp3");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6");

            Label.Checkpoint("bp_test3", "bp_test4", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp3");

                bool isWindows = System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows);
                if (isWindows)
                    Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp6", "./VSCodeTestSrcBreakpointResolve/PROGRAM.CS");
                else
                    Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp6", "./VSCodeTestSrcBreakpointResolve/Program.cs");

                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "resolved_bp4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp7", "Program.cs");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp8", "VSCodeTestSrcBreakpointResolve/Program.cs");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp9", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                int? current_bp_id =  Context.GetBreakpointId(@"__FILE__:__LINE__", "bp9", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                // one more check, that we have proper breakpoint ids
                Assert.Equal(Context.CurrentBpId, current_bp_id, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp7");
Label.Breakpoint("bp8");
Label.Breakpoint("resolved_bp4");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp9");

            Label.Checkpoint("bp_test4", "bp_test_nested", (Object context) => {
                Context Context = (Context)context;
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp4");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp10");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp11");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp12");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp13");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp14");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp15");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp16");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp17");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp18");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp19");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp20");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp21");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp22");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp23");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            VSCodeTestSrcBreakpointResolve2.Program.testfunc();

            // tests resolve for nested methods
                                                                                    Label.Breakpoint("bp10");
            void nested_func1()
            {                                                                       Label.Breakpoint("resloved_bp10");
                Console.WriteLine("Hello World!");                                  
                                                                                    Label.Breakpoint("bp11");
            }                                                                       Label.Breakpoint("resloved_bp11");
            nested_func1();
                                                                                    Label.Breakpoint("bp12");
            void nested_func2()
            {                                                                       Label.Breakpoint("resloved_bp12");
                Console.WriteLine("Hello World!");                                  Label.Breakpoint("bp13");
            }
            nested_func2();
                                                                                    Label.Breakpoint("bp14");
            Console.WriteLine("Hello World!");                                      Label.Breakpoint("resloved_bp14");

            void nested_func3()
            {
                Console.WriteLine("Hello World!");                                  Label.Breakpoint("bp15");
            }
            nested_func3();

            void nested_func4() { }; void nested_func5() { };                       Label.Breakpoint("bp16");
            nested_func4();

            void nested_func6() {
            }; void nested_func7() { };                                             Label.Breakpoint("bp17");
            nested_func6();

            void nested_func8() { }; void nested_func9() {
            }; void nested_func10() { };                                            Label.Breakpoint("bp18");
            nested_func9();

            void nested_func11() { void nested_func12() { void nested_func13() { 
                                                                                    Label.Breakpoint("bp19");
            };                                                                      Label.Breakpoint("resloved_bp19");
            nested_func13(); }; 
            nested_func12(); };
            nested_func11();

            Console.WriteLine("1111Hello World!"); void nested_func14() {           Label.Breakpoint("bp20");
            Console.WriteLine("2222Hello World!");
            };                                                                      Label.Breakpoint("bp22");
            nested_func14();                                                        Label.Breakpoint("bp21");

            Label.Checkpoint("bp_test_nested", "bp_test_constructor", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resloved_bp10");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resloved_bp11");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resloved_bp12");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp13");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resloved_bp14");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp15");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp16");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp17");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp18");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resloved_bp19");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp20");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp21");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp22");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test constructor

            int bp = 23;                                                            Label.Breakpoint("bp23");
            test_constructors test_constr1 = new test_constructors();
            test_constructors test_constr2 = new test_constructors(5);

            Label.Checkpoint("bp_test_constructor", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp23");

                Context.AddManualBreakpoint(@"__FILE__:__LINE__", "Program.cs", 291);
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Program.cs", 291);
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Program.cs", 291);
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
