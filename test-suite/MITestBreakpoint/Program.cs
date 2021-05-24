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

        public void WasBreakpointWithIdHit(string caller_trace, string bpName, string id)
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
                var bkptno = (MIConst)output["bkptno"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    bkptno.CString == id &&
                    line == bp.NumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasFuncBreakpointWithIdHit(string caller_trace, string func_name, string id)
        {
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
                var funcName = ((MIConst)frame["func"]).CString;
                var bkptno = ((MIConst)output["bkptno"]).CString;

                if (funcName.EndsWith(func_name) &&
                    bkptno == id) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
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

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        public ControlInfo ControlInfo { get; private set; }
        public MIDebugger MIDebugger { get; private set; }
        public string id1 = null;
        public string id2 = null;
        public string id3 = null;
        public string id4 = null;
        public string id5 = null;
        public string id6 = null;
        public string id7 = null;
        public string id8 = null;
    }
}

namespace MITestBreakpoint
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "BREAK1_test", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                var insBp1Resp =
                    Context.MIDebugger.Request("3-break-insert -f " +
                        ((LineBreakpoint)Context.ControlInfo.Breakpoints["BREAK1"]).ToString());
                Context.id1 = ((MIConst)((MITuple)insBp1Resp["bkpt"])["number"]).CString;

                var insBp2Resp =
                    Context.MIDebugger.Request("4-break-insert -f " +
                        ((LineBreakpoint)Context.ControlInfo.Breakpoints["BREAK2"]).ToString());
                Context.id2 = ((MIConst)((MITuple)insBp2Resp["bkpt"])["number"]).CString;

                var insBp3Resp =
                    Context.MIDebugger.Request("5-break-insert -f " +
                        ((LineBreakpoint)Context.ControlInfo.Breakpoints["BREAK3"]).ToString());
                Context.id3 = ((MIConst)((MITuple)insBp3Resp["bkpt"])["number"]).CString;
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("6-break-condition " + Context.id3 + " x>20").Class,
                             @"__FILE__:__LINE__");

                var insBp4Resp =
                    Context.MIDebugger.Request("7-break-insert -f -c \"y>200\" " +
                        ((LineBreakpoint)Context.ControlInfo.Breakpoints["BREAK4"]).ToString());
                Context.id4 = ((MIConst)((MITuple)insBp4Resp["bkpt"])["number"]).CString;

                var insBp5Resp =
                    Context.MIDebugger.Request("8-break-insert -f Program.TestFuncBreakpoint");
                Context.id5 = ((MIConst)((MITuple)insBp5Resp["bkpt"])["number"]).CString;

                var insBp6Resp =
                    Context.MIDebugger.Request("9-break-insert -f -c \"z>6\" Program.TestFuncBreakpointCond1");
                Context.id6 = ((MIConst)((MITuple)insBp6Resp["bkpt"])["number"]).CString;

                var insBp7Resp =
                    Context.MIDebugger.Request("10-break-insert -f TestFuncBreakpointCond2(int)");
                Context.id7 = ((MIConst)((MITuple)insBp7Resp["bkpt"])["number"]).CString;
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("11-break-condition " + Context.id7 + " t==50").Class,
                             @"__FILE__:__LINE__");

                var insBp8Resp =
                    Context.MIDebugger.Request("11-break-insert -f TestFuncBreakpointDelete()");
                Context.id8 = ((MIConst)((MITuple)insBp8Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK1");

            Label.Checkpoint("BREAK1_test", "BREAK3_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK1", Context.id1);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("13-break-delete " + Context.id2).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("14-break-delete " + Context.id8).Class,
                             @"__FILE__:__LINE__");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR5");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestFunc(10);
            TestFunc(21);
            TestFunc(9);
            TestFunc2(1);
            TestFunc2(20);
            TestFunc2(300);
            TestFuncBreakpoint();
            TestFuncBreakpointCond1(1);
            TestFuncBreakpointCond1(5);
            TestFuncBreakpointCond1(10);
            TestFuncBreakpointCond2(10);
            TestFuncBreakpointCond2(50);
            TestFuncBreakpointCond2(100);
            TestFuncBreakpointDelete();
            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK2");

            test_attr_func1();
            test_attr_func2();
            test_attr_func3();
            ctest_attr1.test_func();
            ctest_attr2.test_func();

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void TestFunc(int x)
        {
            x++;                                    Label.Breakpoint("BREAK3");

            Label.Checkpoint("BREAK3_test", "BREAK4_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK3", Context.id3);
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void TestFunc2(int y)
        {
            y++;                                    Label.Breakpoint("BREAK4");

            Label.Checkpoint("BREAK4_test", "FUNCBREAK_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK4", Context.id4);
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
        static void TestFuncBreakpoint()
        {
            Console.WriteLine("TestFuncBreakpoint");

            Label.Checkpoint("FUNCBREAK_test", "FUNCBREAKCOND1_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasFuncBreakpointWithIdHit(@"__FILE__:__LINE__", "Program.TestFuncBreakpoint()", Context.id5);
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void TestFuncBreakpointCond1(int z)
        {
            Console.WriteLine("TestFuncBreakpointCond1 " + z.ToString());

            Label.Checkpoint("FUNCBREAKCOND1_test", "FUNCBREAKCOND2_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasFuncBreakpointWithIdHit(@"__FILE__:__LINE__", "Program.TestFuncBreakpointCond1()", Context.id6);
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void TestFuncBreakpointCond2(int t)
        {
            Console.WriteLine("TestFuncBreakpointCond2 " + t.ToString());

            Label.Checkpoint("FUNCBREAKCOND2_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasFuncBreakpointWithIdHit(@"__FILE__:__LINE__", "Program.TestFuncBreakpointCond2()", Context.id7);
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void TestFuncBreakpointDelete()
        {
        }

        [DebuggerStepThroughAttribute()]
        static void test_attr_func1()
        {                                                           Label.Breakpoint("BREAK_ATTR1");
        }

        [DebuggerNonUserCodeAttribute()]
        static void test_attr_func2()
        {                                                           Label.Breakpoint("BREAK_ATTR2");
        }

        [DebuggerHiddenAttribute()]
        static void test_attr_func3()
        {                                                           Label.Breakpoint("BREAK_ATTR3");
        }
    }

    [DebuggerStepThroughAttribute()]
    class ctest_attr1
    {
        public static void test_func()
        {                                                           Label.Breakpoint("BREAK_ATTR4");
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static void test_func()
        {                                                           Label.Breakpoint("BREAK_ATTR5");
        }
    }
}
