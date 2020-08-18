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

        public static void AddBreakpoint(string bpName, string bpPath = null, string Condition = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type);
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

        public static void RemoveBreakpoint(string bpName, string bpPath = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type);
            var lbp = (LineBreakpoint)bp;
            string sourceFile = bpPath != null ? bpPath : lbp.FileName;

            List<SourceBreakpoint> listBp;
            if (!SrcBreakpoints.TryGetValue(sourceFile, out listBp))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

            List<int?> listBpId;
            if (!SrcBreakpointIds.TryGetValue(sourceFile, out listBpId))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

            int indexBp = listBp.FindIndex(x => x.line == lbp.NumLine);
            listBp.RemoveAt(indexBp);
            listBpId.RemoveAt(indexBp);
        }

        public static int? GetBreakpointId(string bpName, string bpPath = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type);
            var lbp = (LineBreakpoint)bp;
            string sourceFile = bpPath != null ? bpPath : lbp.FileName;

            List<SourceBreakpoint> listBp;
            if (!SrcBreakpoints.TryGetValue(sourceFile, out listBp))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

            List<int?> listBpId;
            if (!SrcBreakpointIds.TryGetValue(sourceFile, out listBpId))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

            int indexBp = listBp.FindIndex(x => x.line == lbp.NumLine);
            return listBpId[indexBp];
        }

        public static void SetBreakpoints()
        {
            foreach (var Breakpoints in SrcBreakpoints) {
                SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
                setBreakpointsRequest.arguments.source.name = Path.GetFileName(Breakpoints.Key);

                setBreakpointsRequest.arguments.source.path = Breakpoints.Key;
                setBreakpointsRequest.arguments.breakpoints.AddRange(Breakpoints.Value);
                setBreakpointsRequest.arguments.sourceModified = false;
                var ret = VSCodeDebugger.Request(setBreakpointsRequest);
                Assert.True(ret.Success);

                SetBreakpointsResponse setBreakpointsResponse =
                    JsonConvert.DeserializeObject<SetBreakpointsResponse>(ret.ResponseStr);

                // check, that we don't have hiddenly re-created breakpoints with different ids
                for (int i = 0; i < setBreakpointsResponse.body.breakpoints.Count; i++) {
                    if (SrcBreakpointIds[Breakpoints.Key][i] == null) {
                        CurrentBpId++;
                        SrcBreakpointIds[Breakpoints.Key][i] = setBreakpointsResponse.body.breakpoints[i].id;
                    } else {
                        if (SrcBreakpointIds[Breakpoints.Key][i] != setBreakpointsResponse.body.breakpoints[i].id)
                            throw new NetcoreDbgTestCore.ResultNotSuccessException();
                    }
                }
            }
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
                    && Frame.source.name == lbp.FileName)
                    return;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
        public static int CurrentBpId = 0;
        // Note, SrcBreakpoints and SrcBreakpointIds must have same order of the elements, since we use indexes for mapping.
        public static Dictionary<string, List<SourceBreakpoint>> SrcBreakpoints = new Dictionary<string, List<SourceBreakpoint>>();
        public static Dictionary<string, List<int?>> SrcBreakpointIds = new Dictionary<string, List<int?>>();
    }
}

namespace VSCodeTestSrcBreakpointResolve
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", () => {
                Context.PrepareStart();

                // setup breakpoints before process start
                // in this way we will check breakpoint resolve routine during module load

                Context.AddBreakpoint("bp0_delete_test1");
                Context.AddBreakpoint("bp0_delete_test2");
                Context.AddBreakpoint("bp1");
                Context.AddBreakpoint("bp2", "../Program.cs");
                Context.AddBreakpoint("bp3", "VSCodeTestSrcBreakpointResolve/Program.cs");
                Context.AddBreakpoint("bp4", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                Context.SetBreakpoints();

                Context.RemoveBreakpoint("bp0_delete_test1");
                Context.SetBreakpoints();

                Context.PrepareEnd();
                Context.WasEntryPointHit();

                Context.RemoveBreakpoint("bp0_delete_test2");
                Context.SetBreakpoints();

                Context.Continue();
            });

Label.Breakpoint("bp0_delete_test1");
Label.Breakpoint("bp0_delete_test2");
Label.Breakpoint("bp1");
Label.Breakpoint("bp2");
Label.Breakpoint("bp3");
Label.Breakpoint("resolved_bp1");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp4");

            Label.Checkpoint("bp_test1", "bp_test2", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp1"]);

                // check, that we have proper breakpoint ids
                Context.AddBreakpoint("bp0_delete_test1"); // previously was deleted with id1
                Context.SetBreakpoints();
                int? id7 = Context.GetBreakpointId("bp0_delete_test1");
                Assert.Equal(Context.CurrentBpId, id7);
                Context.RemoveBreakpoint("bp0_delete_test1");
                Context.SetBreakpoints();

                Context.AddBreakpoint("bp5");
                Context.AddBreakpoint("bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Context.SetBreakpoints();
                int? id_bp5_b = Context.GetBreakpointId("bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Assert.Equal(Context.CurrentBpId, id_bp5_b);

                Context.Continue();
            });

Label.Breakpoint("bp5_resolve_wrong_source"); // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
                                        Console.WriteLine("Hello World!");

Label.Breakpoint("bp5");                // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
Label.Breakpoint("resolved_bp2");       Console.WriteLine("Hello World!");

            Label.Checkpoint("bp_test2", "bp_test3", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp2"]);

                Context.RemoveBreakpoint("bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Context.RemoveBreakpoint("bp5");
                Context.SetBreakpoints();

                bool isWindows = System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows);
                if (isWindows)
                    Context.AddBreakpoint("bp6", "./VSCodeTestSrcBreakpointResolve/PROGRAM.CS");
                else
                    Context.AddBreakpoint("bp6", "./VSCodeTestSrcBreakpointResolve/Program.cs");

                Context.AddBreakpoint("bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Context.SetBreakpoints();
                int? id_bp6_b = Context.GetBreakpointId("bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Assert.Equal(Context.CurrentBpId, id_bp6_b);

                Context.Continue();
            });

                                        Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6_resolve_wrong_source");
Label.Breakpoint("resolved_bp3");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6");

            Label.Checkpoint("bp_test3", "bp_test4", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp3"]);

                bool isWindows = System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows);
                if (isWindows)
                    Context.RemoveBreakpoint("bp6", "./VSCodeTestSrcBreakpointResolve/PROGRAM.CS");
                else
                    Context.RemoveBreakpoint("bp6", "./VSCodeTestSrcBreakpointResolve/Program.cs");

                Context.RemoveBreakpoint("bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Context.SetBreakpoints();

                Context.AddBreakpoint("resolved_bp4");
                Context.AddBreakpoint("bp7", "Program.cs");
                Context.AddBreakpoint("bp8", "VSCodeTestSrcBreakpointResolve/Program.cs");
                Context.AddBreakpoint("bp9", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                Context.SetBreakpoints();
                int? current_bp_id =  Context.GetBreakpointId("bp9", "./VSCodeTestSrcBreakpointResolve/folder/../Program.cs");
                // one more check, that we have proper breakpoint ids
                Assert.Equal(Context.CurrentBpId, current_bp_id);

                Context.Continue();
            });

Label.Breakpoint("bp7");
Label.Breakpoint("bp8");
Label.Breakpoint("resolved_bp4");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp9");

            Label.Checkpoint("bp_test4", "finish", () => {
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp4"]);
                Context.Continue();
            });

            VSCodeTestSrcBreakpointResolve2.Program.testfunc();

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
