using System;
using System.IO;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            // Explicitly enable JMC for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set just-my-code 1").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            // Explicitly enable StepFiltering for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set enable-step-filtering 1").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-file-exec-and-symbols " + ControlInfo.CorerunPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-exec-arguments " + ControlInfo.TargetAssemblyPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-run").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasEntryPointHit(string caller_trace)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "entry-point-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var func = (MIConst)frame["func"];
                if (func.CString == ControlInfo.TestName + ".Program.Main()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasStep(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                if (reason.CString != "end-stepping-range") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var line = ((MIConst)frame["line"]).Int;
                if (bp.NumLine == line) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    line == bp.NumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "exited") {
                    return false;
                }

                var exitCode = (MIConst)output["exit-code"];

                if (exitCode.CString == "0") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        bool IsStoppedEvent(MIOutOfBandRecord record)
        {
            if (record.Type != MIOutOfBandRecordType.Async) {
                return false;
            }

            var asyncRecord = (MIAsyncRecord)record;

            if (asyncRecord.Class != MIAsyncRecordClass.Exec ||
                asyncRecord.Output.Class != MIAsyncOutputClass.Stopped) {
                return false;
            }

            return true;
        }

        public void StepOver(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-next").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepIn(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-step").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepOut(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-finish").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
    }
}


namespace MITestStepping
{
    public readonly struct Digit
    {
        private readonly byte digit;

        public Digit(byte digit)
        {
            this.digit = digit;
        }
        public static implicit operator byte(Digit d)
        {
            return d.digit;
        }
        public static explicit operator Digit(byte b)
        {
            return new Digit(b);
        }
    }

    public class TestBreakpointInProperty
    {
        private int _value = 7;

        private int AddOne(int data)
        {
            return data + 1;                                    Label.Breakpoint("test_break_property_getter_1");
        Label.Breakpoint("test_break_property_getter_2");}

        public int Data
        {
            get
            {
                int tmp = AddOne(_value);                       Label.Breakpoint("test_break_property_getter_3");
                return tmp;                                     Label.Breakpoint("test_break_property_getter_4");
            Label.Breakpoint("test_break_property_getter_5");}
            set
            {
                _value = value;                             Label.Breakpoint("test_break_property_setter_1");
            Label.Breakpoint("test_break_property_setter_2");}
        }
    }

    class TestStepInArguments
    {
        public int P1 => 1;
        public int P2 => 2;
        
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

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "step1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "inside_func1_1"); // check, that step-in and breakpoint at same line will generate only one event - step
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "inside_func2_1"); // check, that step-over and breakpoint inside method will generate breakpoint and reset step
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "test_break_property_getter_1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "test_break_property_setter_1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("step 1");                        Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("step 2");                        Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step_in", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("step_in", "step_in_func", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            test_func1();                                        Label.Breakpoint("step_func1");

            Label.Checkpoint("step_over", "step_over_breakpoint", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_func2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            test_func2();                                        Label.Breakpoint("step_func2");

            // Test debugger attribute on methods with JMC enabled.

            test_attr_func1_1();                                            Label.Breakpoint("test_attr_func1_1");
            test_attr_func1_2();                                            Label.Breakpoint("test_attr_func1_2");
            test_attr_func2_1();                                            Label.Breakpoint("test_attr_func2_1");
            test_attr_func2_2();                                            Label.Breakpoint("test_attr_func2_2");
            test_attr_func3_1();                                            Label.Breakpoint("test_attr_func3_1");
            test_attr_func3_2();                                            Label.Breakpoint("test_attr_func3_2");

            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3_2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on class with JMC enabled.

            ctest_attr1.test_func();                                        Label.Breakpoint("test_attr_class1_func");
            ctest_attr2.test_func();                                        Label.Breakpoint("test_attr_class2_func");

            Label.Checkpoint("test_attr2", "test_property_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class1_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // Test step filtering.

            int i1 = test_property1;                                        Label.Breakpoint("test_property1");
            int i2 = test_property2;                                        Label.Breakpoint("test_property2");
            int i3 = test_property3;                                        Label.Breakpoint("test_property3");
            int i4 = test_property4;                                        Label.Breakpoint("test_property4");
            Console.WriteLine("Test debugger attribute on property end.");  Label.Breakpoint("test_step_filtering_end");

            Label.Checkpoint("test_property_attr1", "test_step_through", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_property1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property3");
                Context.StepIn(@"__FILE__:__LINE__");
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
                Context.WasStep(@"__FILE__:__LINE__", "test_step_cast3");
                Context.StepIn(@"__FILE__:__LINE__");

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
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "test_break_property_setter_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_setter_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "test_break_property_getter_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_3");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_getter_5");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_break_property_3");
                Context.StepOver(@"__FILE__:__LINE__");

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

            Label.Checkpoint("test_step_arguments", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_1");
                Context.StepOver(@"__FILE__:__LINE__");

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
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_arguments_M5_3");
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
                Context.StepOut(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static public void test_func1()
        {                                                       Label.Breakpoint("inside_func1_1");
            Console.WriteLine("test_func1");                    Label.Breakpoint("inside_func1_2");

            Label.Checkpoint("step_in_func", "step_out_func", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "inside_func1_1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("step_out_func", "step_over", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "inside_func1_2");
                Context.StepOut(@"__FILE__:__LINE__");
            });
        }

        static public void test_func2()
        {
            Console.WriteLine("test_func2");                    Label.Breakpoint("inside_func2_1");

            Label.Checkpoint("step_over_breakpoint", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "inside_func2_1");
                Context.StepOut(@"__FILE__:__LINE__");
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
        {
        }
        [DebuggerNonUserCodeAttribute()]
        static int test_attr_func2_2()
        {
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
            get { return 2; }
        }

        public static int test_property3
        {
            [DebuggerHiddenAttribute()]
            get { return 3; }
        }

        public static int test_property4
        {
            get { return 4; }
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
        {
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

        static void testGetterStepping()
        {
            Console.WriteLine("code execution inside getter");
        }

        static public TestImpl getImpl1
        {
            get 
            {
                testGetterStepping();
                return impl;
            }
        }

        static public TestImpl getImpl2()
        {                                                                   Label.Breakpoint("test_step_through_getImpl2");
            return impl;
        }
    }
}
