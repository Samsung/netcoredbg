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
        public static void Prepare()
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-file-exec-and-symbols "
                                            + DebuggeeInfo.CorerunPath).Class);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-exec-arguments "
                                            + DebuggeeInfo.TargetAssemblyPath).Class);

            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-run").Class);
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

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestEvalNotEnglish
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "eval_test", () => {
                Context.Prepare();

                Context.WasEntryPointHit();

                LineBreakpoint lbp = (LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK"];

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-break-insert -f " + lbp.FileName
                                                        + ":" + lbp.NumLine).Class);

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("-exec-continue").Class);
            });

            Console.WriteLine("영어 출력이 아닌 테스트.");
            Console.WriteLine("测试非英语输出。");

            int 당신 = 1;
            당신++;                                                      Label.Breakpoint("BREAK");

            Label.Checkpoint("eval_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK"]);

                var notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "你"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class);

                notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "你 + 1"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class);


                var notEnglishVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "당신"));
                Assert.Equal(MIResultClass.Done, notEnglishVariable.Class);
                Assert.Equal("1", ((MIConst)notEnglishVariable["value"]).CString);

                notEnglishVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "당신 + 11"));
                Assert.Equal(MIResultClass.Done, notEnglishVariable.Class);
                Assert.Equal("12", ((MIConst)notEnglishVariable["value"]).CString);

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
