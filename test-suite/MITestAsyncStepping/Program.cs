using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            // Explicitly enable JMC for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set just-my-code 1").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-file-exec-and-symbols " + ControlInfo.CorerunPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-exec-arguments " + ControlInfo.TargetAssemblyPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-run").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasEntryPointHit(string caller_trace)
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

                var frame = (MITuple)output["frame"];
                var func = (MIConst)frame["func"];
                if (func.CString == ControlInfo.TestName + ".Program.<Main>d__0.MoveNext()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasStep(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                if (reason.CString != "end-stepping-range") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var line = ((MIConst)frame["line"]).Int;
                if (bp.NumLine == line) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
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

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    line == bp.NumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        bool IsStoppedEvent(MIOutOfBandRecord record)
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

        public void StepOver(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-next").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepIn(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-step").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void StepOut(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-finish").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
    }
}


namespace MITestAsyncStepping
{
    class Program
    {
       static async Task Main(string[] args)
       {
           Label.Checkpoint("init", "step1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("Before double await block");     Label.Breakpoint("step1");

            Label.Checkpoint("step1", "step2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("step2");

            Label.Checkpoint("step2", "step3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("step3");

            Label.Checkpoint("step3", "step4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("After double await block");      Label.Breakpoint("step4");

            Label.Checkpoint("step4", "step_in_func1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_func1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check step-in and step-out before await blocks (async step-out magic test)
            await test_func1();                                  Label.Breakpoint("step_func1");

            Label.Checkpoint("step_out_func1_check", "step_in_func4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "step_func4");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check Task<TResult>
            await test_func4();                                  Label.Breakpoint("step_func4");

            Label.Checkpoint("step_out_func4_check", "step_in_func2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func4");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "step_func2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check step-in and step-over until we back to caller (async step-out magic test)
            await test_func2();                                  Label.Breakpoint("step_func2");

            Label.Checkpoint("step_out_func2_check", "step_in_func3_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle1");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // check async method call with awaits in cycle
            await test_func3();                                 Label.Breakpoint("step_func3_cycle1");
            await test_func3();                                 Label.Breakpoint("step_func3_cycle2");

            Label.Checkpoint("step_out_func3_check_cycle1", "step_in_func3_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle1");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle2");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // WhenAll
            Task<string> t1 = AddWordAsync("Word2");             Label.Breakpoint("step_whenall_1");
            Task<string> t2 = AddWordAsync("Word3");             Label.Breakpoint("step_whenall_2");
            await Task.WhenAll(t1, t2);                          Label.Breakpoint("step_whenall_3");
            Console.WriteLine(t1.Result + t2.Result);            Label.Breakpoint("step_whenall_4");

            Label.Checkpoint("step_whenall", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "step_func3_cycle2");
                Context.StepOver(@"__FILE__:__LINE__");

                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "step_whenall_4");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on methods with JMC enabled.

            test_attr_func1();                                              Label.Breakpoint("test_attr_func1");
            test_attr_func2();                                              Label.Breakpoint("test_attr_func2");
            test_attr_func3();                                              Label.Breakpoint("test_attr_func3");

            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func1");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func2");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_func3");
                Context.StepIn(@"__FILE__:__LINE__");
            });

            // Test debugger attribute on class with JMC enabled.

            ctest_attr1.test_func();                                        Label.Breakpoint("test_attr_class1_func");
            ctest_attr2.test_func();                                        Label.Breakpoint("test_attr_class2_func");
            Console.WriteLine("Test debugger attribute on methods end.");   Label.Breakpoint("test_attr_end");

            Label.Checkpoint("test_attr2", "test_async_void", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class1_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_class2_func");
                Context.StepIn(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_attr_end");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "test_async_void1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "test_async_void3");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test `async void` stepping.

            await Task.Run((Action)( async () =>
            {
                await Task.Yield();                                        Label.Breakpoint("test_async_void1");
            }));                                                           Label.Breakpoint("test_async_void2");
            await Task.Delay(5000);
            Console.WriteLine("Test debugger `async void` stepping end."); Label.Breakpoint("test_async_void3");

            Label.Checkpoint("test_async_void", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "test_async_void1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "test_async_void2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "test_async_void3");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static async Task test_func1()
        {                                                       Label.Breakpoint("test_func1_step1");

            Label.Checkpoint("step_in_func1", "step_out_func1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func1_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func1");                    Label.Breakpoint("test_func1_step2");

            Label.Checkpoint("step_out_func1", "step_out_func1_check", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func1_step2");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);
        }

        static async Task test_func2()
        {                                                       Label.Breakpoint("test_func2_step1");

            Label.Checkpoint("step_in_func2", "func2_step1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func2");                    Label.Breakpoint("test_func2_step2");

            Label.Checkpoint("func2_step1", "func2_step2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func2_step3");

            Label.Checkpoint("func2_step2", "func2_step3", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("func2_step3", "step_out_func2_check", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func2_step4");
                Context.StepOver(@"__FILE__:__LINE__");
            });
        Label.Breakpoint("test_func2_step4");}

        static async Task test_func3()
        {                                                       Label.Breakpoint("test_func3_step1");

            Label.Checkpoint("step_in_func3_cycle1", "func3_step1_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("step_in_func3_cycle2", "func3_step1_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test_func3");                    Label.Breakpoint("test_func3_step2");

            Label.Checkpoint("func3_step1_cycle1", "func3_step2_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step1_cycle2", "func3_step2_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step3");

            Label.Checkpoint("func3_step2_cycle1", "func3_step3_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step2_cycle2", "func3_step3_cycle2", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step3");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func3_step4");

            Label.Checkpoint("func3_step3_cycle1", "step_out_func3_check_cycle1", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step4");
                Context.StepOut(@"__FILE__:__LINE__");
            });
            Label.Checkpoint("func3_step3_cycle2", "step_whenall", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func3_step4");
                Context.StepOut(@"__FILE__:__LINE__");
            });
        }

        static async Task<int> test_func4()
        {                                                       Label.Breakpoint("test_func4_step1");

            Label.Checkpoint("step_in_func4", "step_over_func4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func4_step1");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func4_step2");

            Label.Checkpoint("step_over_func4", "step_out_func4", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func4_step2");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            await Task.Delay(1500);                             Label.Breakpoint("test_func4_step3");

            Label.Checkpoint("step_out_func4", "step_out_func4_check", (Object context) => {
                Context Context = (Context)context;
                Context.WasStep(@"__FILE__:__LINE__", "test_func4_step3");
                Context.StepOut(@"__FILE__:__LINE__");
            });

            return 5;
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

        [DebuggerStepThroughAttribute()]
        static async Task test_attr_func1()
        {
            await Task.Delay(1500);
        }

        [DebuggerNonUserCodeAttribute()]
        static async Task test_attr_func2()
        {
            await Task.Delay(1500);
        }

        [DebuggerHiddenAttribute()]
        static async Task test_attr_func3()
        {
            await Task.Delay(1500);
        }
    }

    [DebuggerStepThroughAttribute()]
    class ctest_attr1
    {
        public static async Task test_func()
        {
            await Task.Delay(1500);
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static async Task test_func()
        {
            await Task.Delay(1500);
        }
    }
}
