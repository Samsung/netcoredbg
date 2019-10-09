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

        public static bool IsStoppedThreadInList(MIList threads, MIConst threadId)
        {
            var threadsArray = threads.ToArray();
            foreach (var thread in threadsArray) {
                if (((MIConst)((MITuple)thread)["id"]).CString == threadId.CString
                    && ((MIConst)((MITuple)thread)["state"]).CString == "stopped") {
                    return true;
                };
            }
            return false;
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
            Label.Checkpoint("init", "test_steps", () => {
                Context.Prepare();
                Context.WasEntryPointHit();

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("4-exec-step").Class);
            });

            Console.WriteLine("Hello World!");      Label.Breakpoint("STEP1");

            Label.Checkpoint("test_steps", "finish", () => {
                MIConst threadId = Context.WasStep("STEP1");

                // test for "thread-info"
                var res = Context.MIDebugger.Request("5-thread-info");
                Assert.Equal(MIResultClass.Done, res.Class);
                Assert.True(Context.IsStoppedThreadInList((MIList)res["threads"], threadId));

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("6-exec-step").Class);

                Context.WasStep("STEP2");

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("7-exec-step").Class);

                Context.WasStep("STEP3");

                Context.Continue();
            });

            func_test();                            Label.Breakpoint("STEP2");

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

       static void func_test()
       {                                            Label.Breakpoint("STEP3");
           Console.WriteLine("Hello World!");
       }
    }
}
