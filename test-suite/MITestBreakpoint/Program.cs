using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public static bool IsStoppedEvent(MIOutOfBandRecord record)
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

        public static void WasEntryPointHit()
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;

                var reason = (MIConst)output["reason"];
                if (reason.CString != "entry-point-hit") {
                    continue;
                }

                var frame = (MITuple)(output["frame"]);
                var func = (MIConst)(frame["func"]);
                if (func.CString == DebuggeeInfo.TestName + ".Program.Main()") {
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasBreakpointHit(Breakpoint breakpoint, string id)
        {
            var records = MIDebugger.Receive();
            var bp = (LineBreakpoint)breakpoint;

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    continue;
                }

                var frame = (MITuple)(output["frame"]);
                var numLine = ((MIConst)frame["line"]).CString;
                var bkptno = ((MIConst)output["bkptno"]).CString;

                if (numLine == bp.NumLine.ToString() &&
                    bkptno == id) {
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasFuncBreakpointHit(string func_name, string id)
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    continue;
                }

                var frame = (MITuple)(output["frame"]);
                var funcName = ((MIConst)frame["func"]).CString;
                var bkptno = ((MIConst)output["bkptno"]).CString;

                if (funcName.EndsWith(func_name) &&
                    bkptno == id) {
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasExit()
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "exited") {
                    continue;
                }

                var exitCode = (MIConst)output["exit-code"];

                if (exitCode.CString == "0") {
                    return;
                } else {
                    throw new NetcoreDbgTestCore.ResultNotSuccessException();
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void DebuggerExit()
        {
            Assert.Equal(MIResultClass.Exit, Context.MIDebugger.Request("-gdb-exit").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
        public static string id1 = null;
        public static string id2 = null;
        public static string id3 = null;
        public static string id4 = null;
        public static string id5 = null;
        public static string id6 = null;
        public static string id7 = null;
        public static string id8 = null;
    }
}

namespace MITestBreakpoint
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "BREAK1_test", () => {
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("1-file-exec-and-symbols "
                                                        + DebuggeeInfo.CorerunPath).Class);
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("2-exec-arguments "
                                                        + DebuggeeInfo.TargetAssemblyPath).Class);

                var insBp1Resp =
                    Context.MIDebugger.Request("3-break-insert -f " +
                        ((LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK1"]).ToString());
                Context.id1 = ((MIConst)((MITuple)insBp1Resp["bkpt"])["number"]).CString;

                var insBp2Resp =
                    Context.MIDebugger.Request("4-break-insert -f " +
                        ((LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK2"]).ToString());
                Context.id2 = ((MIConst)((MITuple)insBp2Resp["bkpt"])["number"]).CString;

                var insBp3Resp =
                    Context.MIDebugger.Request("5-break-insert -f " +
                        ((LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK3"]).ToString());
                Context.id3 = ((MIConst)((MITuple)insBp3Resp["bkpt"])["number"]).CString;
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("6-break-condition " +
                             Context.id3 +
                             " x>20").Class);

                var insBp4Resp =
                    Context.MIDebugger.Request("7-break-insert -f -c \"y>200\" " +
                        ((LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK4"]).ToString());
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
                             Context.MIDebugger.Request("11-break-condition " +
                             Context.id7 +
                             " t==50").Class);

                var insBp8Resp =
                    Context.MIDebugger.Request("11-break-insert -f TestFuncBreakpointDelete()");
                Context.id8 = ((MIConst)((MITuple)insBp8Resp["bkpt"])["number"]).CString;

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-run").Class);

                Context.WasEntryPointHit();

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("12-exec-continue").Class);
            });

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK1");

            Label.Checkpoint("BREAK1_test", "BREAK3_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK1"], Context.id1);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("13-break-delete " + Context.id2).Class);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("14-break-delete " + Context.id8).Class);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("14-exec-continue").Class);
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

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static void TestFunc(int x)
        {
            x++;                                    Label.Breakpoint("BREAK3");

            Label.Checkpoint("BREAK3_test", "BREAK4_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK3"], Context.id3);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("15-exec-continue").Class);
            });
        }

        static void TestFunc2(int y)
        {
            y++;                                    Label.Breakpoint("BREAK4");

            Label.Checkpoint("BREAK4_test", "FUNCBREAK_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK4"], Context.id4);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("16-exec-continue").Class);
            });
        }
        static void TestFuncBreakpoint()
        {
            Console.WriteLine("TestFuncBreakpoint");

            Label.Checkpoint("FUNCBREAK_test", "FUNCBREAKCOND1_test", () => {
                Context.WasFuncBreakpointHit("Program.TestFuncBreakpoint()", Context.id5);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("17-exec-continue").Class);
            });
        }

        static void TestFuncBreakpointCond1(int z)
        {
            Console.WriteLine("TestFuncBreakpointCond1 " + z.ToString());

            Label.Checkpoint("FUNCBREAKCOND1_test", "FUNCBREAKCOND2_test", () => {
                Context.WasFuncBreakpointHit("Program.TestFuncBreakpointCond1()", Context.id6);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("18-exec-continue").Class);
            });
        }

        static void TestFuncBreakpointCond2(int t)
        {
            Console.WriteLine("TestFuncBreakpointCond2 " + t.ToString());

            Label.Checkpoint("FUNCBREAKCOND2_test", "finish", () => {
                Context.WasFuncBreakpointHit("Program.TestFuncBreakpointCond2()", Context.id7);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("19-exec-continue").Class);
            });
        }

        static void TestFuncBreakpointDelete()
        {
        }
    }
}
