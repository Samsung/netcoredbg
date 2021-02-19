using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

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
                if (func.CString == DebuggeeInfo.TestName + ".Program.<Main>d__0.MoveNext()") {
                    return true;
                }

                return false;
            };

            if (!MIDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasStep(string stepName)
        {
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
                    return true;
                }

                return false;
            };

            if (MIDebugger.IsEventReceived(filter))
                return;

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

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}


namespace MITestAsyncStepping
{
    class Program
    {
       static async Task Main(string[] args)
       {
           Label.Checkpoint("init", "step1", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.StepOver();
            });

            Console.WriteLine("Before double await block");     Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", () => {
                Context.WasStep("step1");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step3", () => {
                Context.WasStep("step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("step3");

            Label.Checkpoint("step3", "step4", () => {
                Context.WasStep("step3");
                Context.StepOver();
            });

            Console.WriteLine("After double await block");      Label.Breakpoint("step4");

            Label.Checkpoint("step4", "step_in_func1", () => {
                Context.WasStep("step4");
                Context.StepOver();
                Context.WasStep("step_func1");
                Context.StepIn();
            });

            // check step-in and step-out before await blocks (async step-out magic test)
            await test_func1();                                  Label.Breakpoint("step_func1");

            Label.Checkpoint("step_out_func1_check", "step_in_func2", () => {
                Context.WasStep("step_func1");
                Context.StepOver();
                Context.WasStep("step_func2");
                Context.StepIn();
            });

            // check step-in and step-over until we back to caller (async step-out magic test)
            await test_func2();                                  Label.Breakpoint("step_func2");

            Label.Checkpoint("step_out_func2_check", "step_in_func3_cycle1", () => {
                Context.WasStep("step_func2");
                Context.StepOver();
                Context.WasStep("step_func3_cycle1");
                Context.StepIn();
            });

            // check async method call with awaits in cycle
            await test_func3();                                 Label.Breakpoint("step_func3_cycle1");
            await test_func3();                                 Label.Breakpoint("step_func3_cycle2");

            Label.Checkpoint("step_out_func3_check_cycle1", "step_in_func3_cycle2", () => {
                Context.WasStep("step_func3_cycle1");
                Context.StepOver();
                Context.WasStep("step_func3_cycle2");
                Context.StepIn();
            });
            Label.Checkpoint("step_out_func3_check_cycle2", "step_whenall", () => {
                Context.WasStep("step_func3_cycle2");
                Context.StepOver();
            });

            // WhenAll
            Task<string> t1 = AddWordAsync("Word2");             Label.Breakpoint("step_whenall_1");
            Task<string> t2 = AddWordAsync("Word3");             Label.Breakpoint("step_whenall_2");
            await Task.WhenAll(t1, t2);                          Label.Breakpoint("step_whenall_3");
            Console.WriteLine(t1.Result + t2.Result);            Label.Breakpoint("step_whenall_4");

            Label.Checkpoint("step_whenall", "finish", () => {
                Context.WasStep("step_whenall_1");
                Context.StepOver();
                Context.WasStep("step_whenall_2");
                Context.StepOver();
                Context.WasStep("step_whenall_3");
                Context.StepOver();
                Context.WasStep("step_whenall_4");
                Context.StepOut();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static async Task test_func1()
        {                                                       Label.Breakpoint("test_func1_step1");

            Label.Checkpoint("step_in_func1", "step_out_func1", () => {
                Context.WasStep("test_func1_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func1");                    Label.Breakpoint("test_func1_step2");

            Label.Checkpoint("step_out_func1", "step_out_func1_check", () => {
                Context.WasStep("test_func1_step2");
                Context.StepOut();
            });

            await Task.Delay(1500);
        }

        static async Task test_func2()
        {                                                       Label.Breakpoint("test_func2_step1");

            Label.Checkpoint("step_in_func2", "func2_step1", () => {
                Context.WasStep("test_func2_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func2");                    Label.Breakpoint("test_func2_step2");

            Label.Checkpoint("func2_step1", "func2_step2", () => {
                Context.WasStep("test_func2_step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func2_step3");

            Label.Checkpoint("func2_step2", "func2_step3", () => {
                Context.WasStep("test_func2_step3");
                Context.StepOver();
            });

            Label.Checkpoint("func2_step3", "step_out_func2_check", () => {
                Context.WasStep("test_func2_step4");
                Context.StepOver();
            });
        Label.Breakpoint("test_func2_step4");}
        static async Task test_func3()
        {                                                       Label.Breakpoint("test_func3_step1");

            Label.Checkpoint("step_in_func3_cycle1", "func3_step1_cycle1", () => {
                Context.WasStep("test_func3_step1");
                Context.StepOver();
            });
            Label.Checkpoint("step_in_func3_cycle2", "func3_step1_cycle2", () => {
                Context.WasStep("test_func3_step1");
                Context.StepOver();
            });

            Console.WriteLine("test_func3");                    Label.Breakpoint("test_func3_step2");

            Label.Checkpoint("func3_step1_cycle1", "func3_step2_cycle1", () => {
                Context.WasStep("test_func3_step2");
                Context.StepOver();
            });
            Label.Checkpoint("func3_step1_cycle2", "func3_step2_cycle2", () => {
                Context.WasStep("test_func3_step2");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step3");

            Label.Checkpoint("func3_step2_cycle1", "func3_step3_cycle1", () => {
                Context.WasStep("test_func3_step3");
                Context.StepOver();
            });
            Label.Checkpoint("func3_step2_cycle2", "func3_step3_cycle2", () => {
                Context.WasStep("test_func3_step3");
                Context.StepOver();
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step4");

            Label.Checkpoint("func3_step3_cycle1", "step_out_func3_check_cycle1", () => {
                Context.WasStep("test_func3_step4");
                Context.StepOut();
            });
            Label.Checkpoint("func3_step3_cycle2", "step_out_func3_check_cycle2", () => {
                Context.WasStep("test_func3_step4");
                Context.StepOut();
            });
        }

        static string AddWord(string Word)
        {
            System.Threading.Thread.Sleep(1500);
            return string.Format("Word1, {0}", Word);
        }

        static Task<string> AddWordAsync(string Word)
        {
            return Task.Run<string>(() =>
            {
                return AddWord(Word);
            });
        }
    }
}
