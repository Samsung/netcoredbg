using System;
using System.IO;
using System.Diagnostics;

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

        public static void WasBreakpointHit(string breakpointName)
        {
            var bp = (LineBreakpoint)DebuggeeInfo.Breakpoints[breakpointName];

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

        public static void WasBreakHit(string breakpointName)
        {
            var bp = (LineBreakpoint)DebuggeeInfo.Breakpoints[breakpointName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                var signal_name = (MIConst)output["signal-name"];

                if (reason.CString != "signal-received" &&
                    signal_name.CString != "SIGINT") {
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

        public static void WasStep(string breakpointName)
        {
            var bp = (LineBreakpoint)DebuggeeInfo.Breakpoints[breakpointName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "end-stepping-range") {
                    return false;
                }

                var frame = (MITuple)(output["frame"]);
                var numLine = (MIConst)(frame["line"]);

                if (numLine.CString == bp.NumLine.ToString()) {
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

        public static void EnableBreakpoint(string bpName)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f "
                                            + lbp.FileName + ":" + lbp.NumLine).Class);
        }

        public static void StepOver()
        {
            Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-next").Class);
        }

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestBreak
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "break_test1", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.EnableBreakpoint("break_test2");
                Context.EnableBreakpoint("break_test3");
                Context.EnableBreakpoint("break_test4");
                Context.EnableBreakpoint("break_test5");
                Context.Continue();
            });

            // Test, that debugger stop at Debugger.Break() in managed code.
            Console.WriteLine("Start test.");
            Debugger.Break();                                             Label.Breakpoint("break_test1");

            Label.Checkpoint("break_test1", "break_test2", () => {
                Context.WasBreakHit("break_test1");
                Context.Continue();
            });

            // Test, that debugger ignore Debugger.Break() on continue in case it already stop at breakpoint at this code line.
            Debugger.Break();                                             Label.Breakpoint("break_test2");

            Label.Checkpoint("break_test2", "break_test3", () => {
                Context.WasBreakpointHit("break_test2");
                Context.Continue();
            });

            // Test, that debugger ignore Debugger.Break() on step in case it already stop at breakpoint at this code line.
            Debugger.Break();                                             Label.Breakpoint("break_test3");
            Console.WriteLine("Next Line");                               Label.Breakpoint("break_test3_nextline");

            Label.Checkpoint("break_test3", "break_test4", () => {
                Context.WasBreakpointHit("break_test3");
                Context.StepOver();
                Context.WasStep("break_test3_nextline");
                Context.Continue();
            });

            // Test, that debugger ignore Debugger.Break() on step in case it already stop at step at this code.
            // Note, since test framework can't operate with columns in code line, we test that debugger stop at
            // step-step-step instead of step-step-break-step.
            int i = 0; Debugger.Break(); i++;                             Label.Breakpoint("break_test4");
            Console.WriteLine("Next Line i=" + i.ToString());             Label.Breakpoint("break_test4_nextline");

            Label.Checkpoint("break_test4", "break_test5", () => {
                Context.WasBreakpointHit("break_test4");
                Context.StepOver();
                Context.WasStep("break_test4");
                Context.StepOver();
                Context.WasStep("break_test4");
                Context.StepOver();
                Context.WasStep("break_test4_nextline");
                Context.Continue();
            });

            // Test, that debugger stop at Debugger.Break() in managed code during step-over and reset step.
            test_func();                                                  Label.Breakpoint("break_test5");

            Label.Checkpoint("break_test5", "break_test5_func", () => {
                Context.WasBreakpointHit("break_test5");
                Context.StepOver();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static void test_func()
        {
            Debugger.Break();                                             Label.Breakpoint("break_test5_func");
            Console.WriteLine("Test function.");

            Label.Checkpoint("break_test5_func", "finish", () => {
                Context.WasBreakHit("break_test5_func");
                Context.Continue();
            });
        }
    }
}
