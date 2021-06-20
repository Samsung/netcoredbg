using System;
using System.IO;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

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
            launchRequest.arguments.justMyCode = true; // Explicitly enable JMC for this test.
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

        public void WasStep(string caller_trace, string bpName)
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

        public void StepOver(string caller_trace)
        {
            NextRequest nextRequest = new NextRequest();
            nextRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(nextRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepIn(string caller_trace)
        {
            StepInRequest stepInRequest = new StepInRequest();
            stepInRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(stepInRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepOut(string caller_trace)
        {
            StepOutRequest stepOutRequest = new StepOutRequest();
            stepOutRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(stepOutRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

namespace VSCodeTestAsyncStepping
{
    class Program
    {
        static async Task Main(string[] args)
        {
            Label.Checkpoint("init", "step1", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("Before double await block");     Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("step3");

            Label.Checkpoint("step3", "step4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("After double await block");      Label.Breakpoint("step4");

            Label.Checkpoint("step4", "step_in_func1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_func1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check step-in and step-out before await blocks (async step-out magic test)
            await test_func1();                                  Label.Breakpoint("step_func1");

            Label.Checkpoint("step_out_func1_check", "step_in_func2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check step-in and step-over until we back to caller (async step-out magic test)
            await test_func2();                                  Label.Breakpoint("step_func2");

            Label.Checkpoint("step_out_func2_check", "step_in_func3_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check async method call with awaits in cycle
            await test_func3();                                 Label.Breakpoint("step_func3_cycle1");
            await test_func3();                                 Label.Breakpoint("step_func3_cycle2");

            Label.Checkpoint("step_out_func3_check_cycle1", "step_in_func3_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // WhenAll
            Task<string> t1 = AddWordAsync("Word2");             Label.Breakpoint("step_whenall_1");
            Task<string> t2 = AddWordAsync("Word3");             Label.Breakpoint("step_whenall_2");
            await Task.WhenAll(t1, t2);                          Label.Breakpoint("step_whenall_3");
            Console.WriteLine(t1.Result + t2.Result);            Label.Breakpoint("step_whenall_4");

            Label.Checkpoint("step_whenall", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_4");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on methods with JMC enabled.

            await test_attr_func1();                                        Label.Breakpoint("test_attr_func1");
            await test_attr_func2();                                        Label.Breakpoint("test_attr_func2");
            await test_attr_func3();                                        Label.Breakpoint("test_attr_func3");

            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on class with JMC enabled.

            await ctest_attr1.test_func();                                  Label.Breakpoint("test_attr_class1_func");
            await ctest_attr2.test_func();                                  Label.Breakpoint("test_attr_class2_func");
            Console.WriteLine("Test debugger attribute on methods end.");   Label.Breakpoint("test_attr_end");

            Label.Checkpoint("test_attr2", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class1_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_end");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static async Task test_func1()
        {                                                       Label.Breakpoint("test_func1_step1");

            Label.Checkpoint("step_in_func1", "step_out_func1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func1_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func1");                    Label.Breakpoint("test_func1_step2");

            Label.Checkpoint("step_out_func1", "step_out_func1_check", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func1_step2");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);
        }

        static async Task test_func2()
        {                                                       Label.Breakpoint("test_func2_step1");

            Label.Checkpoint("step_in_func2", "func2_step1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func2");                    Label.Breakpoint("test_func2_step2");

            Label.Checkpoint("func2_step1", "func2_step2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func2_step3");

            Label.Checkpoint("func2_step2", "func2_step3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("func2_step3", "step_out_func2_check", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step4");
                Context.StepOver(@"__FILE__:__LINE__");
            });
        Label.Breakpoint("test_func2_step4");}

        static async Task test_func3()
        {                                                       Label.Breakpoint("test_func3_step1");

            Label.Checkpoint("step_in_func3_cycle1", "func3_step1_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("step_in_func3_cycle2", "func3_step1_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func3");                    Label.Breakpoint("test_func3_step2");

            Label.Checkpoint("func3_step1_cycle1", "func3_step2_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step1_cycle2", "func3_step2_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step3");

            Label.Checkpoint("func3_step2_cycle1", "func3_step3_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step2_cycle2", "func3_step3_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step4");

            Label.Checkpoint("func3_step3_cycle1", "step_out_func3_check_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step4");
                Context.StepOut(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step3_cycle2", "step_whenall", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step4");
                Context.StepOut(@"__FILE__:__LINE__");
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

        [DebuggerStepThroughAttribute()]
        static async Task test_attr_func1()
        {
            await Task.Delay(1500);
        }

        [DebuggerNonUserCodeAttribute()]
        static async Task test_attr_func2()
        {
            await Task.Delay(1500);
        }

        [DebuggerHiddenAttribute()]
        static async Task test_attr_func3()
        {
            await Task.Delay(1500);
        }
    }

    [DebuggerStepThroughAttribute()]
    class ctest_attr1
    {
        public static async Task test_func()
        {
            await Task.Delay(1500);
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static async Task test_func()
        {
            await Task.Delay(1500);
        }
    }
}
