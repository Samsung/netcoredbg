using System;
using System.IO;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    public static class Context
    {
        // https://docs.microsoft.com/en-us/visualstudio/extensibility/debugger/reference/evalflags
        public enum enum_EVALFLAGS {
            EVAL_RETURNVALUE = 0x0002,
            EVAL_NOSIDEEFFECTS = 0x0004,
            EVAL_ALLOWBPS = 0x0008,
            EVAL_ALLOWERRORREPORT = 0x0010,
            EVAL_FUNCTION_AS_ADDRESS = 0x0040,
            EVAL_NOFUNCEVAL = 0x0080,
            EVAL_NOEVENTS = 0x1000
        }

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

        public static string GetChildValue(string variable, int childIndex, bool setEvalFlags, enum_EVALFLAGS evalFlags)
        {
            var res = MIDebugger.Request("-var-create - * " +
                                         "\"" + variable + "\"" +
                                         (setEvalFlags ? (" --evalFlags " + (int)evalFlags) : "" ),
                                         2000);
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
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "entry-point-hit") {
                    return false;
                }

                var frame = (MITuple)(output["frame"]);
                var func = (MIConst)(frame["func"]);
                if (func.CString == DebuggeeInfo.TestName + ".Program.Main()") {
                    return true;
                }

                return false;
            };

            if (!MIDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasBreakpointHit(Breakpoint breakpoint)
        {
            var bp = (LineBreakpoint)breakpoint;

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    return false;
                }

                var frame = (MITuple)(output["frame"]);
                var fileName = (MIConst)(frame["file"]);
                var numLine = (MIConst)(frame["line"]);

                if (fileName.CString == bp.FileName &&
                    numLine.CString == bp.NumLine.ToString()) {
                    return true;
                }

                return false;
            };

            if (!MIDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasExit()
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

            if (!MIDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void DebuggerExit()
        {
            Assert.Equal(MIResultClass.Exit, Context.MIDebugger.Request("-gdb-exit").Class);
        }

        static bool IsStoppedEvent(MIOutOfBandRecord record)
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

    public struct TestStruct3
    {
        public int val1
        {
            get
            {
                return 777; 
            }
        }
    }

    public struct TestStruct4
    {
        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
        public int val1
        {
            get
            {
                return 666; 
            }
        }

        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.Never)]
        public int val2
        {
            get
            {
                return 777; 
            }
        }

        public int val3
        {
            get
            {
                return 888; 
            }
        }
    }

    public struct TestStruct5
    {
        public int val1
        {
            get
            {
                return 111; 
            }
        }

        public int val2
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 222; 
            }
        }

        public string val3
        {
            get
            {
                return "text_333"; 
            }
        }

        public float val4
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 444.4f; 
            }
        }

        public float val5
        {
            get
            {
                return 555.5f; 
            }
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
                Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK3"], 6);
                Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK4"], 7);
                Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK5"], 8);

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("9-exec-continue").Class);
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

            Label.Checkpoint("test_var", "test_eval_flags", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK2"]);

                Assert.Equal("666", Context.GetChildValue("ts.struct2", 0, false, 0));
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

            TestStruct3 ts3 = new TestStruct3();

            int dummy3 = 3;                                     Label.Breakpoint("BREAK3");

            Label.Checkpoint("test_eval_flags", "test_debugger_browsable_state", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK3"]);

                Context.CreateAndAssignVar("ts3.val1", "666");
                Assert.Equal("777", Context.GetChildValue("ts3", 0, false, 0));
                Context.enum_EVALFLAGS evalFlags = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                Assert.Equal("<error>", Context.GetChildValue("ts3", 0, true, evalFlags));

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("-exec-continue").Class);
            });

            TestStruct4 ts4 = new TestStruct4();

            int dummy4 = 4;                                     Label.Breakpoint("BREAK4");

            Label.Checkpoint("test_debugger_browsable_state", "test_NotifyOfCrossThreadDependency", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK4"]);

                Assert.Equal("666", Context.GetChildValue("ts4", 0, false, 0));
                Assert.Equal("888", Context.GetChildValue("ts4", 1, false, 0));

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("-exec-continue").Class);
            });

            TestStruct5 ts5 = new TestStruct5();

            // part of NotifyOfCrossThreadDependency test, no active evaluation here for sure
            System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();

            int dummy5 = 5;                                     Label.Breakpoint("BREAK5");

            Label.Checkpoint("test_NotifyOfCrossThreadDependency", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK5"]);

                Assert.Equal("111", Context.GetChildValue("ts5", 0, false, 0));
                Assert.Equal("<error>", Context.GetChildValue("ts5", 1, false, 0));
                Assert.Equal("\\\"text_333\\\"", Context.GetChildValue("ts5", 2, false, 0));
                Assert.Equal("<error>", Context.GetChildValue("ts5", 3, false, 0));
                Assert.Equal("555.5", Context.GetChildValue("ts5", 4, false, 0));

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("-exec-continue").Class);
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
