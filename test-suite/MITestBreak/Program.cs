using System;
using System.IO;
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
                if (func.CString == ControlInfo.TestName + ".Program.Main()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void WasBreakHit(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

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

                var frame = (MITuple)output["frame"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    line == bp.NumLine) {
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

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void StepOver(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-next").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
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

namespace MITestBreak
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "break_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "break_test2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "break_test3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "break_test4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "break_test5");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test, that debugger stop at Debugger.Break() in managed code.
            Console.WriteLine("Start test.");
            Debugger.Break();                                             Label.Breakpoint("break_test1");

            Label.Checkpoint("break_test1", "break_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakHit(@"__FILE__:__LINE__", "break_test1");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test, that debugger ignore Debugger.Break() on continue in case it already stop at breakpoint at this code line.
            Debugger.Break();                                             Label.Breakpoint("break_test2");

            Label.Checkpoint("break_test2", "break_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "break_test2");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test, that debugger ignore Debugger.Break() on step in case it already stop at breakpoint at this code line.
            Debugger.Break();                                             Label.Breakpoint("break_test3");
            Console.WriteLine("Next Line");                               Label.Breakpoint("break_test3_nextline");

            Label.Checkpoint("break_test3", "break_test4", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "break_test3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "break_test3_nextline");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test, that debugger ignore Debugger.Break() on step in case it already stop at step at this code.
            // Note, since test framework can't operate with columns in code line, we test that debugger stop at
            // step-step-step instead of step-step-break-step.
            int i = 0; Debugger.Break(); i++;                             Label.Breakpoint("break_test4");
            Console.WriteLine("Next Line i=" + i.ToString());             Label.Breakpoint("break_test4_nextline");

            Label.Checkpoint("break_test4", "break_test5", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "break_test4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "break_test4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "break_test4");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "break_test4_nextline");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test, that debugger stop at Debugger.Break() in managed code during step-over and reset step.
            test_func();                                                  Label.Breakpoint("break_test5");

            Label.Checkpoint("break_test5", "break_test5_func", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "break_test5");
                Context.StepOver(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void test_func()
        {
            Debugger.Break();                                             Label.Breakpoint("break_test5_func");
            Console.WriteLine("Test function.");

            Label.Checkpoint("break_test5_func", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakHit(@"__FILE__:__LINE__", "break_test5_func");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
