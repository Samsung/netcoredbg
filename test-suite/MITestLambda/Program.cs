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

        public static void WasBreakpointHit(Breakpoint breakpoint)
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
                var fileName = (MIConst)(frame["file"]);
                var numLine = (MIConst)(frame["line"]);

                if (fileName.CString == bp.FileName &&
                    numLine.CString == bp.NumLine.ToString()) {
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

        public static void CheckVar(string varName)
        {
            // varName is equal to it value;
            var res = MIDebugger.Request("-var-create - * \"" + varName + "\"");

            if (res.Class != MIResultClass.Done ||
                ((MIConst)res["value"]).String != "\"" + varName + "\"") {
                Logger.LogLine(((MIConst)res["value"]).String);
                Logger.LogLine(varName);
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
            }
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

namespace MITestLambda
{
    delegate void Lambda(string argVar);

    class Program
    {
        static string staticVar = "staticVar";

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "lambda_test", () => {
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("1-file-exec-and-symbols "
                                                        + DebuggeeInfo.CorerunPath).Class);
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("2-exec-arguments "
                                                        + DebuggeeInfo.TargetAssemblyPath).Class);
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("3-exec-run").Class);

                Context.WasEntryPointHit();

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("4-break-insert -f "
                                                        + DebuggeeInfo.Breakpoints["LAMBDAENTRY"]).Class);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("5-exec-continue").Class);
            });
            string mainVar = "mainVar";

            Lambda lambda = (argVar) => {
                string localVar = "localVar";

                Label.Checkpoint("lambda_test", "finish", () => {
                    Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["LAMBDAENTRY"]);

                    Context.CheckVar("staticVar");
                    Context.CheckVar("mainVar");
                    Context.CheckVar("argVar");
                    Context.CheckVar("localVar");

                    Assert.Equal(MIResultClass.Running,
                                 Context.MIDebugger.Request("6-exec-continue").Class);
                });

                Console.WriteLine(staticVar);              Label.Breakpoint("LAMBDAENTRY");
                Console.WriteLine(mainVar);
                Console.WriteLine(argVar);
                Console.WriteLine(localVar);
            };

            lambda("argVar");

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
            });
        }
    }
}
