using System;
using System.IO;
using System.Diagnostics;
using System.Threading;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    class Context
    {
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

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
        public static Process testProcess;
    }
}

namespace MITestTarget
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_setup", () => {
                Context.testProcess = new Process();
                Context.testProcess.StartInfo.UseShellExecute = false;
                Context.testProcess.StartInfo.FileName = DebuggeeInfo.CorerunPath;
                Context.testProcess.StartInfo.Arguments = DebuggeeInfo.TargetAssemblyPath;
                Context.testProcess.StartInfo.CreateNoWindow = true;
                Assert.True(Context.testProcess.Start());
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-target-attach "
                                                        + Context.testProcess.Id.ToString()).Class);
            });

            // wait some time, control process should attach and setup breakpoints
            Thread.Sleep(3000);

            Label.Checkpoint("bp_setup", "bp_test", () => {
                Context.EnableBreakpoint("bp");
                Context.EnableBreakpoint("bp2");
            });

            int i = 10000;
            i++;                                                          Label.Breakpoint("bp");

            Label.Checkpoint("bp_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp"]);
                Context.Continue();
            });

            // wait some time after process detached
            Thread.Sleep(i);                                              Label.Breakpoint("bp2");

            Label.Checkpoint("finish", "", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp2"]);

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-thread-info").Class);
                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-target-detach").Class);
                Assert.Equal(MIResultClass.Error, Context.MIDebugger.Request("-thread-info").Class);

                Assert.False(Context.testProcess.HasExited);
                Context.testProcess.Kill();
                while (!Context.testProcess.HasExited) {};
                // killed by SIGKILL
                Assert.NotEqual(0, Context.testProcess.ExitCode);

                Context.DebuggerExit();
            });
        }
    }
}
