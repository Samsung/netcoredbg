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

        public static void EnableBreakpoint(string bpName)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f "
                                            + lbp.FileName + ":" + lbp.NumLine).Class);
        }

        public static void WasStep(Breakpoint breakpoint)
        {
            var records = MIDebugger.Receive();
            var bp = (LineBreakpoint)breakpoint;

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                if (reason.CString != "end-stepping-range") {
                    continue;
                }

                var frame = (MITuple)output["frame"];
                var line = ((MIConst)frame["line"]).Int;
                if (bp.NumLine == line) {
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }
        
        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestExecFinish
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp1_test", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.EnableBreakpoint("bp1");
                Context.Continue();
            });

            test_exec_finish();                                             Label.Breakpoint("bp2");

            Label.Checkpoint("bp2_test", "finish", () => {
                Context.WasStep(DebuggeeInfo.Breakpoints["bp2"]);
                Context.Continue();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
            });
        }

        public static void test_exec_finish()
        {
            Console.WriteLine("test_exec_finish line 1");
            Console.WriteLine("test_exec_finish line 2");
            Console.WriteLine("test_exec_finish line 3");                   Label.Breakpoint("bp1");
            Console.WriteLine("test_exec_finish line 4");

            Label.Checkpoint("bp1_test", "bp2_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp1"]);
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-finish").Class);
            });
        }
    }
}
