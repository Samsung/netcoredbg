using System;
using System.IO;
using System.Diagnostics;
using System.Threading;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
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

        public ControlInfo ControlInfo { get; private set; }
        public MIDebugger MIDebugger { get; private set; }
        public Process testProcess;
    }
}

namespace MITestTarget
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_setup", (Object context) => {
                Context Context = (Context)context;
                Context.testProcess = new Process();
                Context.testProcess.StartInfo.UseShellExecute = false;
                Context.testProcess.StartInfo.FileName = Context.ControlInfo.CorerunPath;
                Context.testProcess.StartInfo.Arguments = Context.ControlInfo.TargetAssemblyPath;
                Context.testProcess.StartInfo.CreateNoWindow = true;
                Assert.True(Context.testProcess.Start(), @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-target-attach " + Context.testProcess.Id.ToString()).Class,
                             @"__FILE__:__LINE__");
            });

            // wait some time, control process should attach and setup breakpoints
            Thread.Sleep(3000);

            Label.Checkpoint("bp_setup", "bp_test", (Object context) => {
                Context Context = (Context)context;
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp2");
            });

            int i = 10000;
            i++;                                                          Label.Breakpoint("bp");

            Label.Checkpoint("bp_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // wait some time after process detached
            Thread.Sleep(i);                                              Label.Breakpoint("bp2");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp2");

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-thread-info").Class, @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-target-detach").Class, @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Error, Context.MIDebugger.Request("-thread-info").Class, @"__FILE__:__LINE__");

                Assert.False(Context.testProcess.HasExited, @"__FILE__:__LINE__");
                Context.testProcess.Kill();
                while (!Context.testProcess.HasExited) {};
                // killed by SIGKILL
                Assert.NotEqual(0, Context.testProcess.ExitCode, @"__FILE__:__LINE__");

                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
