using System;
using System.IO;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

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

        public static void WasStep(string BpName)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "step")) {

                    // In case of async method, thread could be changed, care about this.
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

            Breakpoint breakpoint = DebuggeeInfo.Breakpoints[BpName];
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

        public static void StepOver()
        {
            NextRequest nextRequest = new NextRequest();
            nextRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(nextRequest).Success);
        }

        public static void StepIn()
        {
            StepInRequest stepInRequest = new StepInRequest();
            stepInRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(stepInRequest).Success);
        }

        public static void StepOut()
        {
            StepOutRequest stepOutRequest = new StepOutRequest();
            stepOutRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(stepOutRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
        public static string BreakpointSourceName;
        public static List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        public static List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestAsyncStepping
{
    class Program
    {
        static async Task Main(string[] args)
        {
            Label.Checkpoint("init", "step1", () => {
                Context.PrepareStart();
                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.StepOver();
            });

            Console.WriteLine("Before double await block");     Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", () => {
                Context.WasStep("step1");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step3", () => {
                Context.WasStep("step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("step3");

            Label.Checkpoint("step3", "step4", () => {
                Context.WasStep("step3");
                Context.StepOver();
            });

            Console.WriteLine("After double await block");      Label.Breakpoint("step4");

            Label.Checkpoint("step4", "step_in_func1", () => {
                Context.WasStep("step4");
                Context.StepOver();
                Context.WasStep("step_func1");
                Context.StepIn();
            });

            // check step-in and step-out before await blocks (async step-out magic test)
            await test_func1();                                  Label.Breakpoint("step_func1");

            Label.Checkpoint("step_out_func1_check", "step_in_func2", () => {
                Context.WasStep("step_func1");
                Context.StepOver();
                Context.WasStep("step_func2");
                Context.StepIn();
            });

            // check step-in and step-over until we back to caller (async step-out magic test)
            await test_func2();                                  Label.Breakpoint("step_func2");

            Label.Checkpoint("step_out_func2_check", "step_in_func3_cycle1", () => {
                Context.WasStep("step_func2");
                Context.StepOver();
                Context.WasStep("step_func3_cycle1");
                Context.StepIn();
            });

            // check async method call with awaits in cycle
            await test_func3();                                 Label.Breakpoint("step_func3_cycle1");
            await test_func3();                                 Label.Breakpoint("step_func3_cycle2");

            Label.Checkpoint("step_out_func3_check_cycle1", "step_in_func3_cycle2", () => {
                Context.WasStep("step_func3_cycle1");
                Context.StepOver();
                Context.WasStep("step_func3_cycle2");
                Context.StepIn();
            });
            Label.Checkpoint("step_out_func3_check_cycle2", "step_whenall", () => {
                Context.WasStep("step_func3_cycle2");
                Context.StepOver();
            });

            // WhenAll
            Task<string> t1 = AddWordAsync("Word2");             Label.Breakpoint("step_whenall_1");
            Task<string> t2 = AddWordAsync("Word3");             Label.Breakpoint("step_whenall_2");
            await Task.WhenAll(t1, t2);                          Label.Breakpoint("step_whenall_3");
            Console.WriteLine(t1.Result + t2.Result);            Label.Breakpoint("step_whenall_4");

            Label.Checkpoint("step_whenall", "finish", () => {
                Context.WasStep("step_whenall_1");
                Context.StepOver();
                Context.WasStep("step_whenall_2");
                Context.StepOver();
                Context.WasStep("step_whenall_3");
                Context.StepOver();
                Context.WasStep("step_whenall_4");
                Context.StepOut();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static async Task test_func1()
        {                                                       Label.Breakpoint("test_func1_step1");

            Label.Checkpoint("step_in_func1", "step_out_func1", () => {
                Context.WasStep("test_func1_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func1");                    Label.Breakpoint("test_func1_step2");

            Label.Checkpoint("step_out_func1", "step_out_func1_check", () => {
                Context.WasStep("test_func1_step2");
                Context.StepOut();
            });

            await Task.Delay(1500);
        }

        static async Task test_func2()
        {                                                       Label.Breakpoint("test_func2_step1");

            Label.Checkpoint("step_in_func2", "func2_step1", () => {
                Context.WasStep("test_func2_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func2");                    Label.Breakpoint("test_func2_step2");

            Label.Checkpoint("func2_step1", "func2_step2", () => {
                Context.WasStep("test_func2_step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func2_step3");

            Label.Checkpoint("func2_step2", "func2_step3", () => {
                Context.WasStep("test_func2_step3");
                Context.StepOver();
            });

            Label.Checkpoint("func2_step3", "step_out_func2_check", () => {
                Context.WasStep("test_func2_step4");
                Context.StepOver();
            });
        Label.Breakpoint("test_func2_step4");}
        static async Task test_func3()
        {                                                       Label.Breakpoint("test_func3_step1");

            Label.Checkpoint("step_in_func3_cycle1", "func3_step1_cycle1", () => {
                Context.WasStep("test_func3_step1");
                Context.StepOver();
            });
            Label.Checkpoint("step_in_func3_cycle2", "func3_step1_cycle2", () => {
                Context.WasStep("test_func3_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func3");                    Label.Breakpoint("test_func3_step2");

            Label.Checkpoint("func3_step1_cycle1", "func3_step2_cycle1", () => {
                Context.WasStep("test_func3_step2");
                Context.StepOver();
            });
            Label.Checkpoint("func3_step1_cycle2", "func3_step2_cycle2", () => {
                Context.WasStep("test_func3_step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step3");

            Label.Checkpoint("func3_step2_cycle1", "func3_step3_cycle1", () => {
                Context.WasStep("test_func3_step3");
                Context.StepOver();
            });
            Label.Checkpoint("func3_step2_cycle2", "func3_step3_cycle2", () => {
                Context.WasStep("test_func3_step3");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step4");

            Label.Checkpoint("func3_step3_cycle1", "step_out_func3_check_cycle1", () => {
                Context.WasStep("test_func3_step4");
                Context.StepOut();
            });
            Label.Checkpoint("func3_step3_cycle2", "step_out_func3_check_cycle2", () => {
                Context.WasStep("test_func3_step4");
                Context.StepOut();
            });
        }

        static string AddWord(string Word)
        {
            System.Threading.Thread.Sleep(1500);
            return string.Format("Word1, {0}", Word);
        }

        static Task<string> AddWordAsync(string Word)
        {
            return Task.Run<string>(() =>
            {
                return AddWord(Word);
            });
        }
    }
}
