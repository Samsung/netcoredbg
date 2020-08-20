using System;
using System.IO;
using System.Threading;

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

                // we don't check exit code here, since Windows and Linux provide different exit code in case of "-gdb-exit" usage
                return true;
            };

            if (!MIDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestGDB
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.Continue();

                var showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class);
                Assert.Equal("1", ((MIConst)showResult["value"]).CString);

                // NOTE space is only legit delimiter (name-value) for gdb-set (see miengine for more info)
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-gdb-set just-my-code 0").Class);

                showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class);
                Assert.Equal("0", ((MIConst)showResult["value"]).CString);

                // NOTE space is only legit delimiter (name-value) for gdb-set (see miengine for more info)
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-gdb-set just-my-code 1").Class);

                showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class);
                Assert.Equal("1", ((MIConst)showResult["value"]).CString);

                Assert.Equal(MIResultClass.Exit,
                             Context.MIDebugger.Request("-gdb-exit").Class);

                Context.WasExit();
            });

            Thread.Sleep(10000);
            Console.WriteLine("Hello World!");
        }
    }
}
