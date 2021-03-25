using System;
using System.IO;

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

        public string EnableBreakpoint(string caller_trace, string bpName, string bpPath = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            var BpResp =  MIDebugger.Request("-break-insert -f " + (bpPath != null ? bpPath : lbp.FileName)  + ":" + lbp.NumLine);

            Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            CurrentBpId++;

            // return breakpoint id
            return ((MIConst)((MITuple)BpResp["bkpt"])["number"]).CString;
        }

        public void DeleteBreakpoint(string caller_trace, string id)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-delete " + id).Class,
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
        public MIDebugger MIDebugger { get; private set; }
        public int CurrentBpId = 0;
        public string id_bp5;
        public string id_bp5_b;
        public string id_bp6;
        public string id_bp6_b;
    }
}

namespace MITestSrcBreakpointResolve
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", (Object context) => {
                Context Context = (Context)context;
                // setup breakpoints before process start
                // in this way we will check breakpoint resolve routine during module load

                var id1 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1");
                var id2 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp2", "../Program.cs");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp3", "MITestSrcBreakpointResolve/Program.cs");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp4", "./MITestSrcBreakpointResolve/folder/../Program.cs");

                Context.DeleteBreakpoint(@"__FILE__:__LINE__", id1);

                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Context.DeleteBreakpoint(@"__FILE__:__LINE__", id2);

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp0_delete_test1");
Label.Breakpoint("bp0_delete_test2");
Label.Breakpoint("bp1");
Label.Breakpoint("bp2");
Label.Breakpoint("bp3");
Label.Breakpoint("resolved_bp1");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp4");

            Label.Checkpoint("bp_test1", "bp_test2", (Object context) => {
                Context Context = (Context)context;
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp1");

                // check, that we have proper breakpoint ids (check, that for MI/GDB resolved breakpoints were not re-created hiddenly with different id)
                var id7 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp0_delete_test1"); // previously was deleted with id1
                Assert.Equal(Context.CurrentBpId.ToString(), id7, @"__FILE__:__LINE__");
                Context.DeleteBreakpoint(@"__FILE__:__LINE__", id7);

                Context.id_bp5_b = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp5_b, @"__FILE__:__LINE__");

                Context.id_bp5 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp5");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp5, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp5_resolve_wrong_source"); // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
                                        Console.WriteLine("Hello World!");

Label.Breakpoint("bp5");                // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
Label.Breakpoint("resolved_bp2");       Console.WriteLine("Hello World!");

            Label.Checkpoint("bp_test2", "bp_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp2");

                Context.DeleteBreakpoint(@"__FILE__:__LINE__", Context.id_bp5);
                Context.DeleteBreakpoint(@"__FILE__:__LINE__", Context.id_bp5_b);

                Context.id_bp6_b = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp6_b, @"__FILE__:__LINE__");
    
                Context.id_bp6 = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp6");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp6, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

                                        Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6_resolve_wrong_source");
Label.Breakpoint("resolved_bp3");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6");

            Label.Checkpoint("bp_test3", "bp_test4", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp3");

                Context.DeleteBreakpoint(@"__FILE__:__LINE__", Context.id_bp6);
                Context.DeleteBreakpoint(@"__FILE__:__LINE__", Context.id_bp6_b);

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "resolved_bp4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp7", "Program.cs");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp8", "MITestSrcBreakpointResolve/Program.cs");
                var current_bp_id = Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp9", "./MITestSrcBreakpointResolve/folder/../Program.cs");

                // one more check, that we have proper breakpoint ids for MI/GDB
                Assert.Equal(Context.CurrentBpId.ToString(), current_bp_id, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

Label.Breakpoint("bp7");
Label.Breakpoint("bp8");
Label.Breakpoint("resolved_bp4");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp9");

            Label.Checkpoint("bp_test4", "finish", (Object context) => {
                Context Context = (Context)context;
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "resolved_bp4");
                Context.Continue(@"__FILE__:__LINE__");
            });

            MITestSrcBreakpointResolve2.Program.testfunc();

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
