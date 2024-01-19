using System;
using System.IO;
using System.Collections.Generic;
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
            launchRequest.arguments.justMyCode = false; // Explicitly disabled JMC for this test.
            launchRequest.arguments.enableStepFiltering = false; // Explicitly disabled StepFiltering for this test.
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


namespace VSCodeTestNoJMCNoFilterStepping
{
    public readonly struct Digit
    {
        private readonly byte digit;

        public Digit(byte digit)
        {
            this.digit = digit;
        }
        public static implicit operator byte(Digit d)
        {                                                       Label.Breakpoint("test_op_implicit_1");
            return d.digit;                                     Label.Breakpoint("test_op_implicit_2");
        Label.Breakpoint("test_op_implicit_3");}
        public static explicit operator Digit(byte b)
        {                                                       Label.Breakpoint("test_op_explicit_1");
            return new Digit(b);                                Label.Breakpoint("test_op_explicit_2");
        Label.Breakpoint("test_op_explicit_3");}
    }

    public class TestBreakpointInProperty
    {
        private int _value = 7;

        private int AddOne(int data)
        {                                                       Label.Breakpoint("test_break_property_getter_1");
            return data + 1;                                    Label.Breakpoint("test_break_property_getter_2");
        Label.Breakpoint("test_break_property_getter_3");}

        public int Data
        {
            [DebuggerStepThrough]
            get
            {
                int tmp = AddOne(_value);
                return tmp;
            }
            [DebuggerStepThrough]
            set
            {
                _value = value;
            }
        }
    }

    class TestStepInArguments
    {
        public int P1
        {
            get
            {                                                               Label.Breakpoint("test_step_arguments_P1_1");
                return 1;                                                   Label.Breakpoint("test_step_arguments_P1_2");
            Label.Breakpoint("test_step_arguments_P1_3");}
        }
        public int P2
        {
            get
            {                                                               Label.Breakpoint("test_step_arguments_P2_1");
                return 1;                                                   Label.Breakpoint("test_step_arguments_P2_2");
            Label.Breakpoint("test_step_arguments_P2_3");}
        }
        
        public int M1()
        {                                                                   Label.Breakpoint("test_step_arguments_M1_1");
            return 1;                                                       Label.Breakpoint("test_step_arguments_M1_2");
        Label.Breakpoint("test_step_arguments_M1_3");}
        public int M2()
        {                                                                   Label.Breakpoint("test_step_arguments_M2_1");
            return 2;                                                       Label.Breakpoint("test_step_arguments_M2_2");
        Label.Breakpoint("test_step_arguments_M2_3");}

        public void M3(int a, int b)
        {                                                                   Label.Breakpoint("test_step_arguments_M3_1");
            ;                                                               Label.Breakpoint("test_step_arguments_M3_2");
        Label.Breakpoint("test_step_arguments_M3_3");}
        public void M4(int a, int b, int c = 0, int d = 0)
        {                                                                   Label.Breakpoint("test_step_arguments_M4_1");
            ;                                                               Label.Breakpoint("test_step_arguments_M4_2");
        Label.Breakpoint("test_step_arguments_M4_3");}
        public int M5(int k)
        {                                                                   Label.Breakpoint("test_step_arguments_M5_1");
            return k + 1;                                                   Label.Breakpoint("test_step_arguments_M5_2");
        Label.Breakpoint("test_step_arguments_M5_3");}

        [DebuggerStepThrough]
        public int M6()
        {
            return 1;
        }
    }

    class TestStepWithCompilerGenCode : IDisposable
    {
        public void Dispose()
        {                                                                   Label.Breakpoint("test_step_comp_Dispose_1");
            ;                                                               Label.Breakpoint("test_step_comp_Dispose_2");
        Label.Breakpoint("test_step_comp_Dispose_3");}

        public static void M()
        {                                                                   Label.Breakpoint("test_step_comp_M_1");
            Label.Breakpoint("test_step_comp_M_2");using (var c = new TestStepWithCompilerGenCode())
            {                                                               Label.Breakpoint("test_step_comp_M_3");
                ;                                                           Label.Breakpoint("test_step_comp_M_4");
            Label.Breakpoint("test_step_comp_M_5");}
            using var c2 = new TestStepWithCompilerGenCode();               Label.Breakpoint("test_step_comp_M_6");
            try
            {                                                               Label.Breakpoint("test_step_comp_M_7");
                ;                                                           Label.Breakpoint("test_step_comp_M_8");
            Label.Breakpoint("test_step_comp_M_9");}
            finally
            {                                                               Label.Breakpoint("test_step_comp_M_10");
                ;                                                           Label.Breakpoint("test_step_comp_M_11");
            Label.Breakpoint("test_step_comp_M_12");}
        Label.Breakpoint("test_step_comp_M_13");}
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on methods with JMC disabled.

            test_attr_func1_1();                                            Label.Breakpoint("test_attr_func1_1");
            test_attr_func1_2();                                            Label.Breakpoint("test_attr_func1_2");

            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1_2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            test_attr_func2_1();                                            Label.Breakpoint("test_attr_func2_1");
            test_attr_func2_2();                                            Label.Breakpoint("test_attr_func2_2");

            Label.Checkpoint("test_attr2", "test_attr3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_1_in");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_2_in");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            test_attr_func3_1();                                            Label.Breakpoint("test_attr_func3_1");
            test_attr_func3_2();                                            Label.Breakpoint("test_attr_func3_2");

            Label.Checkpoint("test_attr3", "test_attr4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3_2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on class with JMC disabled.

            ctest_attr1.test_func();                                        Label.Breakpoint("test_attr_class1_func");

            Label.Checkpoint("test_attr4", "test_attr5", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class1_func");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            ctest_attr2.test_func();                                        Label.Breakpoint("test_attr_class2_func");

            Label.Checkpoint("test_attr5", "test_property_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func_in");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            // Test step filtering disabled.

            int i1 = test_property1;                                        Label.Breakpoint("test_property1");

            Label.Checkpoint("test_property_attr1", "test_property_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_property1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            int i2 = test_property2;                                        Label.Breakpoint("test_property2");

            Label.Checkpoint("test_property_attr2", "test_property_attr3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_property2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property2_in");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            int i3 = test_property3;                                        Label.Breakpoint("test_property3");

            Label.Checkpoint("test_property_attr3", "test_property_attr4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_property2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property3");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            int i4 = test_property4;                                        Label.Breakpoint("test_property4");
            Console.WriteLine("Test debugger attribute on property end.");  Label.Breakpoint("test_step_filtering_end");

            Label.Checkpoint("test_property_attr4", "test_step_through", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_property4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property4_in");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_filtering_end");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test step through.

            int res = TestImplHolder.getImpl1.Calc1();                      Label.Breakpoint("test_step_through1");
            res = TestImplHolder.getImpl2().Calc1();                        Label.Breakpoint("test_step_through2");
            Console.WriteLine("Test step through end.");                    Label.Breakpoint("test_step_through_end");

            Label.Checkpoint("test_step_through", "test_step_cast", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through_getImpl1");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through_Calc1");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_through2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through_getImpl2");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through_Calc1");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_through2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_through_end");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test steps for casts.

            var d = new Digit(100);                                         Label.Breakpoint("test_step_cast1");
            byte byte_var = d;                                              Label.Breakpoint("test_step_cast2");
            Digit digit_var = (Digit)byte_var;                              Label.Breakpoint("test_step_cast3");
            Console.WriteLine("Test steps for casts end.");                 Label.Breakpoint("test_step_cast_end");

            Label.Checkpoint("test_step_cast", "test_step_breakpoint", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_implicit_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_implicit_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_implicit_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_explicit_1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_explicit_2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_op_explicit_3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast3");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast_end");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test steps with breakpoint in filtered methods.

            var test_obj = new TestBreakpointInProperty();                  Label.Breakpoint("test_break_property_1");
            test_obj.Data = 5;                                              Label.Breakpoint("test_break_property_2");
            int i = test_obj.Data;                                          Label.Breakpoint("test_break_property_3");
            Console.WriteLine("Test steps with breakpoint end.");           Label.Breakpoint("test_step_breakpoint_end");

            Label.Checkpoint("test_step_breakpoint", "test_step_arguments", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_2");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_3");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_breakpoint_end");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test step-in into method arguments.

            TestStepInArguments C = new TestStepInArguments();              Label.Breakpoint("test_step_arguments_1");
            C.M3(C.P1, C.P2);                                               Label.Breakpoint("test_step_arguments_2");
            C.M4(C.M1(), C.M2(), C.M1());                                   Label.Breakpoint("test_step_arguments_3");
            C.M3(C.M5(C.P1), C.M5(C.P1));                                   Label.Breakpoint("test_step_arguments_4");
            C.M6();                                                         Label.Breakpoint("test_step_arguments_5");
            C.M3(C.M6(), C.M6());                                           Label.Breakpoint("test_step_arguments_6");
            Console.WriteLine("Test steps for arguments end.");             Label.Breakpoint("test_step_arguments_end");

            Label.Checkpoint("test_step_arguments", "test_step_comp", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P2_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P2_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P2_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_2");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M2_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M2_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M2_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M1_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M4_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M4_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M4_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_3");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_P1_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_4");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_5");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_6");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M3_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_6");
                Context.StepIn(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_end");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test step-in/step-over for compiler generated code inside user code.

            TestStepWithCompilerGenCode.M();                                Label.Breakpoint("test_step_comp_1");
            TestStepWithCompilerGenCode.M();                                Label.Breakpoint("test_step_comp_2");
            Console.WriteLine("Test steps end.");                           Label.Breakpoint("test_step_comp_end");

            Label.Checkpoint("test_step_comp", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_5");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_6");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_7");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_8");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_9");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_10");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_11");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_12");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_13");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_5");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_5");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_6");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_7");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_8");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_9");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_10");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_11");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_12");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_13");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_Dispose_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_M_13");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_step_comp_end");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        [DebuggerStepThroughAttribute()]
        static void test_attr_func1_1()
        {
        }
        [DebuggerStepThroughAttribute()]
        static int test_attr_func1_2()
        {
            return 5;
        }

        [DebuggerNonUserCodeAttribute()]
        static void test_attr_func2_1()
        {                                              Label.Breakpoint("test_attr_func2_1_in");
        }
        [DebuggerNonUserCodeAttribute()]
        static int test_attr_func2_2()
        {                                              Label.Breakpoint("test_attr_func2_2_in");
            return 5;
        }

        [DebuggerHiddenAttribute()]
        static void test_attr_func3_1()
        {
        }
        [DebuggerHiddenAttribute()]
        static int test_attr_func3_2()
        {
            return 5;
        }

        public static int test_property1
        {
            [DebuggerStepThroughAttribute()]
            get { return 1; }
        }

        public static int test_property2
        {
            [DebuggerNonUserCodeAttribute()]
            get {                                      Label.Breakpoint("test_property2_in");
                 return 2; }
        }

        public static int test_property3
        {
            [DebuggerHiddenAttribute()]
            get { return 3; }
        }

        public static int test_property4
        {
            get {                                      Label.Breakpoint("test_property4_in");
                 return 4; }
        }
    }

    [DebuggerStepThroughAttribute()]
    class ctest_attr1
    {
        public static void test_func()
        {
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static void test_func()
        {                                              Label.Breakpoint("test_attr_class2_func_in");
        }
    }

    public class TestImpl
    {
        public int Calc1()
        {                                                                   Label.Breakpoint("test_step_through_Calc1");
            return 5;
        }
    }

    public class TestImplHolder
    {
        static TestImpl impl = new TestImpl();

        static public TestImpl getImpl1
        {
            get 
            {                                                               Label.Breakpoint("test_step_through_getImpl1");
                return impl;
            }
        }

        static public TestImpl getImpl2()
        {                                                                   Label.Breakpoint("test_step_through_getImpl2");
            return impl;
        }
    }
}
