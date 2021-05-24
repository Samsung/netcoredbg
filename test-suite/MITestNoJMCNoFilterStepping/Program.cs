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
                         MIDebugger.Request("-gdb-set just-my-code 0").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            // Explicitly enable StepFiltering for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set enable-step-filtering 0").Class,
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


namespace MITestNoJMCNoFilterStepping
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on methods with JMC disabled.

            test_attr_func1();                                              Label.Breakpoint("test_attr_func1");

            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            test_attr_func2();                                              Label.Breakpoint("test_attr_func2");

            Label.Checkpoint("test_attr2", "test_attr3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2_in");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            test_attr_func3();                                              Label.Breakpoint("test_attr_func3");

            Label.Checkpoint("test_attr3", "test_attr4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3");
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
                Context.WasStep(@"__FILE__:__LINE__", "test_property3");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            int i4 = test_property4;                                        Label.Breakpoint("test_property4");
            Console.WriteLine("Test debugger attribute on property end.");  Label.Breakpoint("test_step_filtering_end");

            Label.Checkpoint("test_property_attr4", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_property4");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_property4_in");
                Context.StepOut(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_step_filtering_end");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        [DebuggerStepThroughAttribute()]
        static void test_attr_func1()
        {
        }

        [DebuggerNonUserCodeAttribute()]
        static void test_attr_func2()
        {                                              Label.Breakpoint("test_attr_func2_in");
        }

        [DebuggerHiddenAttribute()]
        static void test_attr_func3()
        {
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
}
