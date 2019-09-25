using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    public static class Context
    {
        public static void InsertBreakpoint(Breakpoint bp, int token)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request(token.ToString() + "-break-insert -f "
                                            + bp.ToString()).Class);
        }

        public static void CreateAndAssignVar(string variable, string val)
        {
            var res = MIDebugger.Request("-var-create - * \"" + variable + "\"");
            Assert.Equal(MIResultClass.Done, res.Class);

            string internalName = ((MIConst)res["name"]).CString;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-var-assign " + internalName + " "
                                            + "\"" + val + "\"").Class);
        }

        public static void CreateAndCompareVar(string variable, string val)
        {
            var res = MIDebugger.Request("-var-create - * \"" + variable + "\"");
            Assert.Equal(MIResultClass.Done, res.Class);

            Assert.Equal(val, ((MIConst)res["value"]).CString);
        }

        public static string GetChildValue(string variable, int childIndex)
        {
            var res = MIDebugger.Request("-var-create - * " +
                                         "\"" + variable + "\"", 2000);
            Assert.Equal(MIResultClass.Done, res.Class);

            string struct2 = ((MIConst)res["name"]).CString;

            res = MIDebugger.Request("-var-list-children --simple-values " +
                                         "\"" + struct2 + "\"");
            Assert.Equal(MIResultClass.Done, res.Class);

            var children = (MIList)res["children"];
            var child =  (MITuple)((MIResult)children[childIndex]).Value;

            return ((MIConst)child["value"]).CString;
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

        public static void WasBreakpointHit(Breakpoint bp)
        {
            var records = MIDebugger.Receive();
            var bpLine = ((LineBreakpoint)bp).NumLine.ToString();

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
                var line = (MIConst)(frame["line"]);

                if (bpLine == line.CString) {
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

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestSetValue
{
    public struct TestStruct1
    {
        public int val1;
        public byte val2;

        public TestStruct1(int v1, byte v2)
        {
            val1 = v1;
            val2 = v2;
        }
    }

    public struct TestStruct2
    {
        public int val1;
        public TestStruct1 struct2;

        public TestStruct2(int v1, int v2, byte v3)
        {
            val1 = v1;
            struct2.val1 = v2;
            struct2.val2 = v3;
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "setup_var", () => {
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("1-file-exec-and-symbols "
                                                        + DebuggeeInfo.CorerunPath).Class);
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("2-exec-arguments "
                                                        + DebuggeeInfo.TargetAssemblyPath).Class);
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("3-exec-run").Class);

                Context.WasEntryPointHit();

                Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK1"], 4);
                Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK2"], 5);

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("6-exec-continue").Class);
            });

            TestStruct2 ts = new TestStruct2(1, 5, 10);

            bool testBool = false;
            char testChar = 'ㅎ';
            byte testByte = (byte)10;
            sbyte testSByte = (sbyte)-100;
            short testShort = (short)-500;
            ushort testUShort = (ushort)500;
            int testInt = -999999;
            uint testUInt = 999999;
            long testLong = -999999999;
            ulong testULong = 9999999999;

            decimal b = 0000001.000000000000000000000000006M;
            int[] arrs = decimal.GetBits(b);
            string testString = "someNewString that I'll test with";

            int dummy1 = 1;                                     Label.Breakpoint("BREAK1");

            Label.Checkpoint("setup_var", "test_var", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK1"]);

                Context.CreateAndAssignVar("ts.struct2.val1", "666");
                Context.CreateAndAssignVar("testBool", "true");
                Context.CreateAndAssignVar("testChar", "'a'");
                Context.CreateAndAssignVar("testByte", "200");
                Context.CreateAndAssignVar("testSByte", "-1");
                Context.CreateAndAssignVar("testShort", "-666");
                Context.CreateAndAssignVar("testUShort", "666");
                Context.CreateAndAssignVar("testInt", "666666");
                Context.CreateAndAssignVar("testUInt", "666666");
                Context.CreateAndAssignVar("testLong", "-666666666");
                Context.CreateAndAssignVar("testULong", "666666666");
                Context.CreateAndAssignVar("b", "-1.000000000000000000000017M");
                Context.CreateAndAssignVar("testString", "\"edited string\"");

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("34-exec-continue").Class);
            });

            int dummy2 = 2;                                     Label.Breakpoint("BREAK2");

            Label.Checkpoint("test_var", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK2"]);

                Assert.Equal("666", Context.GetChildValue("ts.struct2", 0));
                Context.CreateAndCompareVar("testBool", "true");
                Context.CreateAndCompareVar("testChar", "97 'a'");
                Context.CreateAndCompareVar("testByte", "200");
                Context.CreateAndCompareVar("testSByte", "-1");
                Context.CreateAndCompareVar("testShort", "-666");
                Context.CreateAndCompareVar("testUShort", "666");
                Context.CreateAndCompareVar("testInt", "666666");
                Context.CreateAndCompareVar("testUInt", "666666");
                Context.CreateAndCompareVar("testLong", "-666666666");
                Context.CreateAndCompareVar("testULong", "666666666");
                Context.CreateAndCompareVar("b", "-1.000000000000000000000017");
                Context.CreateAndCompareVar("testString", "\\\"edited string\\\"");

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("49-exec-continue").Class);
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
