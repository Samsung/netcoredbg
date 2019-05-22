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
        public static string id1;
        public static string id2;
        public static string id3;

        public static string InsertBreakpoint(Breakpoint bp, int token)
        {
            var res = MIDebugger.Request(token.ToString() + "-break-insert -f " +
                                         bp.ToString());

            if (res.Class != MIResultClass.Done) {
                return null;
            }

            return ((MIConst)((MITuple)res["bkpt"])["number"]).CString;
        }

        public static string CalcExpression(string expr, int token)
        {
            var res = MIDebugger.Request(token.ToString() +
                                         "-var-create - * \"" + expr + "\"");

            Assert.Equal(MIResultClass.Done, res.Class);

            return ((MIConst)res["value"]).CString;
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

        public static string GetBreakpointHitId(Breakpoint bp)
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
                    return ((MIConst)output["bkptno"]).CString;
                }
            }

            return "-1";
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

namespace MITestExpression
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "expression_test1", () => {
                Context.MIDebugger.Request("1-file-exec-and-symbols " + DebuggeeInfo.CorerunPath);
                Context.MIDebugger.Request("2-exec-arguments " + DebuggeeInfo.TargetAssemblyPath);

                Context.id1 = Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK1"], 3);
                Assert.NotNull(Context.id1);

                Context.id2 = Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK2"], 4);
                Assert.NotNull(Context.id2);

                Context.id3 = Context.InsertBreakpoint(DebuggeeInfo.Breakpoints["BREAK3"], 5);
                Assert.NotNull(Context.id3);

                Context.MIDebugger.Request("6-exec-run");

                Context.WasEntryPointHit();

                Context.MIDebugger.Request("7-exec-continue");
            });

            int a = 10;
            int b = 11;
            TestStruct tc = new TestStruct(a + 1, b);
            string str1 = "string1";
            string str2 = "string2";
            int c = tc.b + b;                                   Label.Breakpoint("BREAK1");

            Label.Checkpoint("expression_test1", "expression_test2", () => {
                Assert.Equal(Context.id1, Context.GetBreakpointHitId(DebuggeeInfo.Breakpoints["BREAK1"]));

                Assert.Equal("21", Context.CalcExpression("a + b", 8));
                Assert.Equal("22", Context.CalcExpression("tc.a + b", 9));
                Assert.Equal("\\\"string1string2\\\"", Context.CalcExpression("str1 + str2", 10));

                Context.MIDebugger.Request("11-exec-continue");
            });

            int d = 99;
            int e = c + a;                                      Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test2", "expression_test3", () => {
                Assert.Equal(Context.id2, Context.GetBreakpointHitId(DebuggeeInfo.Breakpoints["BREAK2"]));

                Assert.Equal("109", Context.CalcExpression("d + a", 12));

                Context.MIDebugger.Request("13-exec-continue");
            });

            Console.WriteLine(str1 + str2);

            tc.IncA();

            Console.WriteLine("Hello World!");

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
            });
        }

        struct TestStruct
        {
            public int a;
            public int b;

            public TestStruct(int x, int y)
            {
                a = x;
                b = y;
            }

            public void IncA()
            {
                a++;                                            Label.Breakpoint("BREAK3");

                Label.Checkpoint("expression_test3", "finish", () => {
                    Assert.Equal(Context.id3, Context.GetBreakpointHitId(DebuggeeInfo.Breakpoints["BREAK3"]));

                    Assert.Equal("12", Context.CalcExpression("a + 1", 12));

                    Context.MIDebugger.Request("15-exec-continue");
                });
            }
        }
    }
}
