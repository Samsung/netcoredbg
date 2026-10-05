using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        // https://learn.microsoft.com/en-us/visualstudio/extensibility/debugger/reference/evalflags?view=visualstudio&tabs=cpp
        public enum enum_EVALFLAGS {
            EVAL_NONE = 0,
            EVAL_NOFUNCEVAL = 0x0080
        }

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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        string CreateVarRequest(string Expression, enum_EVALFLAGS evalFlags)
        {
            return String.Format("-var-create - * \"{0}\"", Expression) +
                   (evalFlags != enum_EVALFLAGS.EVAL_NONE ? (" --evalFlags " + (int)evalFlags) : "");
        }

        public string GetAndCheckValue(string caller_trace, enum_EVALFLAGS evalFlags, string Expression, string ExpectedType, string ExpectedResult)
        {
            var res = MIDebugger.Request(CreateVarRequest(Expression, evalFlags));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(ExpectedType, ((MIConst)res["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal("editable", ((MIConst)res["attributes"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            return ((MIConst)res["name"]).CString;
        }

        public void CheckErrorAtRequest(string caller_trace, enum_EVALFLAGS evalFlags, string Expression)
        {
            var res = MIDebugger.Request(CreateVarRequest(Expression, evalFlags));
            Assert.Equal(MIResultClass.Error, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetAndCheckStaticMemberValue(string caller_trace, enum_EVALFLAGS evalFlags, string VariableName, string MemberName,
                                                 string ExpectedType, string ExpectedResult)
        {
            var res = MIDebugger.Request(CreateVarRequest(VariableName, evalFlags));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            res = MIDebugger.Request("-var-list-children --simple-values \"" + ((MIConst)res["name"]).CString + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string staticsVarName = "";
            var children = (MIList)res["children"];
            for (int i = 0; i < ((MIConst)res["numchild"]).Int; i++)
            {
                var child = (MITuple)((MIResult)children[i]).Value;
                if (((MIConst)child["exp"]).CString == "Static members")
                    staticsVarName = ((MIConst)child["name"]).CString;
            }
            Assert.True(staticsVarName != "", @"__FILE__:__LINE__"+"\n"+caller_trace);

            res = MIDebugger.Request("-var-list-children --simple-values \"" + staticsVarName + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            children = (MIList)res["children"];
            for (int i = 0; i < ((MIConst)res["numchild"]).Int; i++)
            {
                var child = (MITuple)((MIResult)children[i]).Value;
                if (((MIConst)child["exp"]).CString == MemberName)
                {
                    Assert.Equal(ExpectedType, ((MIConst)child["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(ExpectedResult, ((MIConst)child["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AssignVar(string caller_trace, string varName, string val)
        {
            var res = MIDebugger.Request(String.Format("-var-assign {0} \"{1}\"", varName, val));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(val, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void ErrorAtAssignVar(string caller_trace, string varName, string val, string errMsg)
        {
            var res = MIDebugger.Request(String.Format("-var-assign {0} \"{1}\"", varName, val));
            Assert.Equal(MIResultClass.Error, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(errMsg, ((MIConst)res["msg"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger { get; private set; }
    }
}

namespace MITestStaticFields
{
    // Note, types below don't have static constructor (no static field initializers and no explicit static constructor),
    // so, debugger don't need evaluation for their static fields access.

    public class NoCctor
    {
        public static int s_int;
        [ThreadStatic] public static int t_int;
        [ThreadStatic] public static string t_string;

        public int field_i = 1;
    }

    // Thread static fields with storage, that allocated at first access by thread.
    public class NoCctorThreadStorage
    {
        [ThreadStatic] public static long t_long0;
        [ThreadStatic] public static long t_long1;
        [ThreadStatic] public static long t_long2;
        [ThreadStatic] public static long t_long3;
        [ThreadStatic] public static long t_long4;
        [ThreadStatic] public static long t_long5;
        [ThreadStatic] public static long t_long6;
        [ThreadStatic] public static long t_long7;
    }

    // Not used by program code at all.
    public class NoCctorUntouched
    {
        public static int s_int;
    }

    // Not used by program code at all.
    public class NoCctorUntouchedAssign
    {
        public static int s_int;
    }

    public class Counters
    {
        public static int cctor_runs;
    }

    // Type with static constructor, debugger should run it before static fields access (type object creation).
    public class WithCctor
    {
        public static int s_int = 5;

        static WithCctor()
        {
            Counters.cctor_runs++;
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_main", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_main");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_main_alloc");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_worker");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_cctor");
                Context.Continue(@"__FILE__:__LINE__");
            });

            NoCctor.s_int = 11;
            NoCctor.t_int = 21;
            NoCctor.t_string = "t_string";
            NoCctorThreadStorage.t_long0 = 60;
            NoCctor noCctor = new NoCctor();

            long allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy1 = 1;                                                 Label.Breakpoint("bp_main");
            long allocMain = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_main", "bp_main_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_main");

                var noFuncEval = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                var defaultFlags = Context.enum_EVALFLAGS.EVAL_NONE;
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctor.s_int", "int", "11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctor.s_int", "int", "11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctor.t_int", "int", "21");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctor.t_int", "int", "21");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctor.t_string", "string", "\\\"t_string\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctorThreadStorage.t_long0", "long", "60");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctorUntouched.s_int", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctorUntouched.s_int", "int", "0");
                Context.GetAndCheckStaticMemberValue(@"__FILE__:__LINE__", defaultFlags, "noCctor", "s_int", "int", "11");
                Context.GetAndCheckStaticMemberValue(@"__FILE__:__LINE__", noFuncEval, "noCctor", "t_int", "int", "21");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                                 Label.Breakpoint("bp_main_alloc");

            Label.Checkpoint("bp_main_alloc", "bp_worker", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_main_alloc");

                // No type object creation (no allocation by this thread) during "bp_main" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", Context.enum_EVALFLAGS.EVAL_NONE, "allocMain", "long", "0");

                // Change static field of type that was never used by program code (debugger use type object for this).
                var noFuncEval = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                string varName = Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctorUntouchedAssign.s_int", "int", "0");
                Context.AssignVar(@"__FILE__:__LINE__", varName, "5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctorUntouchedAssign.s_int", "int", "5");

                Context.Continue(@"__FILE__:__LINE__");
            });

            System.Threading.Thread workerThread = new System.Threading.Thread(WorkerThread);
            workerThread.Start();
            workerThread.Join();

            int dummy3 = 3;                                                 Label.Breakpoint("bp_cctor");

            Label.Checkpoint("bp_cctor", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_cctor");

                // Type with static constructor: static constructor run before static field access, even with EVAL_NOFUNCEVAL.
                var noFuncEval = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "Counters.cctor_runs", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "WithCctor.s_int", "int", "5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "Counters.cctor_runs", "int", "1");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void WorkerThread()
        {
            NoCctor noCctor = new NoCctor();

            int dummy1 = 1;                                                 Label.Breakpoint("bp_worker");

            Label.Checkpoint("bp_worker", "bp_cctor", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_worker");

                // Thread static fields never accessed by this thread have default value and can't be changed (debugger use type
                // object, but storage is still not allocated).
                var noFuncEval = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                var defaultFlags = Context.enum_EVALFLAGS.EVAL_NONE;
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctor.s_int", "int", "11");
                // Note, depending on runtime, storage for small thread static field could be preallocated for all threads, value
                // is default in both cases.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctor.t_int", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctor.t_string", "string", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctor.t_string", "string", "null");
                string varName = Context.GetAndCheckValue(@"__FILE__:__LINE__", noFuncEval, "NoCctorThreadStorage.t_long0", "long", "0");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", varName, "5", "'NoCctorThreadStorage.t_long0' cannot be assigned to");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", defaultFlags, "NoCctorThreadStorage.t_long0", "long", "0");
                Context.GetAndCheckStaticMemberValue(@"__FILE__:__LINE__", noFuncEval, "noCctor", "t_string", "string", "null");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
