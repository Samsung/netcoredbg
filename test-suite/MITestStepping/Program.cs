using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    // Context includes methods and constants which
    // will be move to debugger API
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

        public static MIConst WasStep(string stepName)
        {
            MIConst Result = new MIConst("-1");
            var bp = (LineBreakpoint)DebuggeeInfo.Breakpoints[stepName];

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
                    Result = (MIConst)output["thread-id"];
                    return true;
                }

                return false;
            };

            if (MIDebugger.IsEventReceived(filter))
                return Result;

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasBreakpointHit(string bpName)
        {
            MIConst Result = new MIConst("-1");
            var bp = (LineBreakpoint)DebuggeeInfo.Breakpoints[bpName];

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

        public static void EnableBreakpoint(string bpName)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f "
                                            + lbp.FileName + ":" + lbp.NumLine).Class);
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

        public static void StepOver()
        {
            Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-next").Class);
        }

        public static void StepIn()
        {
            Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-step").Class);
        }

        public static void StepOut()
        {
            Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-finish").Class);
        }

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}


namespace MITestStepping
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "step1", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.EnableBreakpoint("inside_func1_1"); // check, that step-in and breakpoint at same line will generate only one event - step
                Context.EnableBreakpoint("inside_func2_1"); // check, that step-over and breakpoint inside method will generate breakpoint and reset step
                Context.StepOver();
            });

            Console.WriteLine("step 1");                        Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", () => {
                Context.WasStep("step1");
                Context.StepOver();
            });

            Console.WriteLine("step 2");                        Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step_in", () => {
                Context.WasStep("step2");
                Context.StepIn();
            });

            Label.Checkpoint("step_in", "step_in_func", () => {
                Context.WasStep("step_func1");
                Context.StepIn();
            });

            test_func1();                                        Label.Breakpoint("step_func1");

            Label.Checkpoint("step_out_check", "step_over", () => {
                Context.WasStep("step_func1");
                Context.StepOver();
            });

            Label.Checkpoint("step_over", "step_over_breakpoint", () => {
                Context.WasStep("step_func2");
                Context.StepOver();
            });

            test_func2();                                        Label.Breakpoint("step_func2");

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static public void test_func1()
        {                                                       Label.Breakpoint("inside_func1_1");
            Console.WriteLine("test_func1");                    Label.Breakpoint("inside_func1_2");

            Label.Checkpoint("step_in_func", "step_out_func", () => {
                Context.WasStep("inside_func1_1");
                Context.StepOver();
            });

            Label.Checkpoint("step_out_func", "step_out_check", () => {
                Context.WasStep("inside_func1_2");
                Context.StepOut();
            });
        }

        static public void test_func2()
        {
            Console.WriteLine("test_func2");                    Label.Breakpoint("inside_func2_1");

            Label.Checkpoint("step_over_breakpoint", "finish", () => {
                Context.WasBreakpointHit("inside_func2_1");
                Context.Continue();
            });
        }
    }
}
