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

namespace MITestVarObject
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", () => {
                Context.Prepare();

                Context.WasEntryPointHit();

                LineBreakpoint lbp = (LineBreakpoint)DebuggeeInfo.Breakpoints["BREAK"];

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("4-break-insert -f " + lbp.FileName
                                                        + ":" + lbp.NumLine).Class);

                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("5-exec-continue").Class);
            });

            decimal d = 12345678901234567890123456m;
            decimal long_zero_dec = 0.00000000000000000017M;
            decimal short_zero_dec = 0.17M;
            int x = 1; Label.Breakpoint("BREAK");

            Label.Checkpoint("values_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["BREAK"]);

                var createDResult =
                    Context.MIDebugger.Request(String.Format("6-var-create - * \"{0}\"", "d"));
                Assert.Equal(MIResultClass.Done, createDResult.Class);

                string d_varName = ((MIConst)createDResult["name"]).CString;
                Assert.Equal("12345678901234567890123456", ((MIConst)createDResult["value"]).CString);
                Assert.Equal("d", ((MIConst)createDResult["exp"]).CString);
                Assert.Equal("0", ((MIConst)createDResult["numchild"]).CString);
                Assert.Equal("decimal", ((MIConst)createDResult["type"]).CString);


                var createLongZeroDecResult =
                    Context.MIDebugger.Request(String.Format("7-var-create 8 * \"{0}\"", "long_zero_dec"));
                Assert.Equal(MIResultClass.Done, createLongZeroDecResult.Class);

                Assert.Equal("0.00000000000000000017", ((MIConst)createLongZeroDecResult["value"]).CString);


                var createShortZeroDecResult =
                    Context.MIDebugger.Request(String.Format("8-var-create 8 * \"{0}\"", "short_zero_dec"));
                Assert.Equal(MIResultClass.Done, createShortZeroDecResult.Class);

                Assert.Equal("0.17", ((MIConst)createShortZeroDecResult["value"]).CString);


                var attrDResult = Context.MIDebugger.Request("9-var-show-attributes " + d_varName);
                Assert.Equal(MIResultClass.Done, attrDResult.Class);
                Assert.Equal("editable", ((MIConst)attrDResult["status"]).CString);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("10-var-delete " + d_varName).Class);

                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request("11-var-show-attributes " + d_varName).Class);


                Assert.Equal(MIResultClass.Running,
                             Context.MIDebugger.Request("12-exec-continue").Class);
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
