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

        public static void WasBreakpointHit(Breakpoint breakpoint)
        {
            var bp = (LineBreakpoint)breakpoint;

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

        public static void DebuggerExit()
        {
            Assert.Equal(MIResultClass.Exit, Context.MIDebugger.Request("-gdb-exit").Class);
        }

        public static string EnableBreakpoint(string bpName, string bpPath = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type);

            var lbp = (LineBreakpoint)bp;

            var BpResp =  MIDebugger.Request("-break-insert -f " + (bpPath != null ? bpPath : lbp.FileName)  + ":" + lbp.NumLine);

            Assert.Equal(MIResultClass.Done, BpResp.Class);

            CurrentBpId++;

            // return breakpoint id
            return ((MIConst)((MITuple)BpResp["bkpt"])["number"]).CString;
        }

        public static void DeleteBreakpoint(string id)
        {
            Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-break-delete " + id).Class);
        }

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
        public static int CurrentBpId = 0;
        public static string id_bp5;
        public static string id_bp5_b;
        public static string id_bp6;
        public static string id_bp6_b;
    }
}

namespace MITestSrcBreakpointResolve
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", () => {
                // setup breakpoints before process start
                // in this way we will check breakpoint resolve routine during module load

                var id1 = Context.EnableBreakpoint("bp0_delete_test1");
                var id2 = Context.EnableBreakpoint("bp0_delete_test2");
                Context.EnableBreakpoint("bp1");
                Context.EnableBreakpoint("bp2", "../Program.cs");
                Context.EnableBreakpoint("bp3", "MITestSrcBreakpointResolve/Program.cs");
                Context.EnableBreakpoint("bp4", "./MITestSrcBreakpointResolve/folder/../Program.cs");

                Context.DeleteBreakpoint(id1);

                Context.Prepare();
                Context.WasEntryPointHit();

                Context.DeleteBreakpoint(id2);

                Context.Continue();
            });

Label.Breakpoint("bp0_delete_test1");
Label.Breakpoint("bp0_delete_test2");
Label.Breakpoint("bp1");
Label.Breakpoint("bp2");
Label.Breakpoint("bp3");
Label.Breakpoint("resolved_bp1");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp4");

            Label.Checkpoint("bp_test1", "bp_test2", () => {
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp1"]);

                // check, that we have proper breakpoint ids (check, that for MI/GDB resolved breakpoints were not re-created hiddenly with different id)
                var id7 = Context.EnableBreakpoint("bp0_delete_test1"); // previously was deleted with id1
                Assert.Equal(Context.CurrentBpId.ToString(), id7);
                Context.DeleteBreakpoint(id7);

                Context.id_bp5_b = Context.EnableBreakpoint("bp5_resolve_wrong_source", "../wrong_folder/./Program.cs");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp5_b);

                Context.id_bp5 = Context.EnableBreakpoint("bp5");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp5);

                Context.Continue();
            });

Label.Breakpoint("bp5_resolve_wrong_source"); // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
                                        Console.WriteLine("Hello World!");

Label.Breakpoint("bp5");                // Console.WriteLine("Hello World!");
                                        /* Console.WriteLine("Hello World!"); */
Label.Breakpoint("resolved_bp2");       Console.WriteLine("Hello World!");

            Label.Checkpoint("bp_test2", "bp_test3", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp2"]);

                Context.DeleteBreakpoint(Context.id_bp5);
                Context.DeleteBreakpoint(Context.id_bp5_b);

                Context.id_bp6_b = Context.EnableBreakpoint("bp6_resolve_wrong_source", "./wrong_folder/Program.cs");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp6_b);
    
                Context.id_bp6 = Context.EnableBreakpoint("bp6");
                Assert.Equal(Context.CurrentBpId.ToString(), Context.id_bp6);

                Context.Continue();
            });

                                        Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6_resolve_wrong_source");
Label.Breakpoint("resolved_bp3");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp6");

            Label.Checkpoint("bp_test3", "bp_test4", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp3"]);

                Context.DeleteBreakpoint(Context.id_bp6);
                Context.DeleteBreakpoint(Context.id_bp6_b);

                Context.EnableBreakpoint("resolved_bp4");
                Context.EnableBreakpoint("bp7", "Program.cs");
                Context.EnableBreakpoint("bp8", "MITestSrcBreakpointResolve/Program.cs");
                var current_bp_id = Context.EnableBreakpoint("bp9", "./MITestSrcBreakpointResolve/folder/../Program.cs");

                // one more check, that we have proper breakpoint ids for MI/GDB
                Assert.Equal(Context.CurrentBpId.ToString(), current_bp_id);

                Context.Continue();
            });

Label.Breakpoint("bp7");
Label.Breakpoint("bp8");
Label.Breakpoint("resolved_bp4");       Console.WriteLine(
                                                          "Hello World!");          Label.Breakpoint("bp9");

            Label.Checkpoint("bp_test4", "finish", () => {
                // check, that actually we have only one active breakpoint per line
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["resolved_bp4"]);
                Context.Continue();
            });

            MITestSrcBreakpointResolve2.Program.testfunc();

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
