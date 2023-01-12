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

        public void WasBreakpointWithIdHit(string caller_trace, string bpName, string id)
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
                var bkptno = (MIConst)output["bkptno"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    bkptno.CString == id &&
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

        public string EnableBreakpoint(string caller_trace, string bpName, int realLineNum = 0)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;
            if (realLineNum == 0)
                realLineNum = lbp.NumLine;

            var insBpResp = MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + realLineNum);
            Assert.Equal(MIResultClass.Done, insBpResp.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            return ((MIConst)((MITuple)insBpResp["bkpt"])["number"]).CString;
        }

        public void UpdateBreakpoint(string caller_trace, string bpID, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;
            
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-update-line " + bpID + " " + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void UpdateBreakpointWithError(string caller_trace, string bpID, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;
            
            Assert.Equal(MIResultClass.Error,
                         MIDebugger.Request("-break-update-line " + bpID + " " + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        public ControlInfo ControlInfo { get; private set; }
        public MIDebugger MIDebugger { get; private set; }
        public string id1 = null;
        public string id2 = null;
        public string id3 = null;
        public string id4 = null;
    }
}

namespace MITestBreakpointUpdate
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_update1", (Object context) => {
                Context Context = (Context)context;

                Context.id1 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.UpdateBreakpoint(@"__FILE__:__LINE__", Context.id1, "BREAK2");

                Context.id2 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK3");

                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Context.UpdateBreakpoint(@"__FILE__:__LINE__", Context.id2, "BREAK4");

                Context.id3 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK5");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-break-update-line " + Context.id3 + " 10000").Class,
                             @"__FILE__:__LINE__");

                Context.id4 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK6", 9000);
                Context.UpdateBreakpoint(@"__FILE__:__LINE__", Context.id4, "BREAK6");

                Context.UpdateBreakpointWithError(@"__FILE__:__LINE__", "1000", "BREAK1"); // Test with not exist breakpoint ID.

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test for breakpoint set and update before process creation and initial breakpoint resolve.

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK1");
            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK2");

            Label.Checkpoint("test_update1", "test_update2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK2", Context.id1);

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test for breakpoint set before process creation and initial breakpoint resolve and update after it was resolved.

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK3");
            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK4");

            Label.Checkpoint("test_update2", "test_update3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK4", Context.id2);

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test for breakpoint set and resolved and updated to line out of source file.

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK5");

            // Test for breakpoint set to line out of source file and update with resolve.

            Console.WriteLine("Hello World!");      Label.Breakpoint("BREAK6");

            Label.Checkpoint("test_update3", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointWithIdHit(@"__FILE__:__LINE__", "BREAK6", Context.id4);

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
