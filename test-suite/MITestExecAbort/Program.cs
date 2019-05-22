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

        public static void WasEntryPointHit()
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;

                var reason = (MIConst)output["reason"];

                if (reason.CString != "entry-point-hit") {
                    continue;
                }

                var frame = (MITuple)(output["frame"]);
                var func = (MIConst)(frame["func"]);
                if (func.CString == DebuggeeInfo.TestName + ".Program.Main()") {
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestExecAbort
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.Continue();

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-thread-info").Class);

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("-exec-abort").Class);

                Assert.Equal(MIResultClass.Error, Context.MIDebugger.Request("-thread-info").Class);
            });

            Thread.Sleep(10000);
            Console.WriteLine("Hello World!");
        }
    }
}
