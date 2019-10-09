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

        public static void Continue()
        {
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-continue").Class);
        }

        public static MIDebugger MIDebugger = new MIDebugger();
    }
}

namespace MITestEnv
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", () => {
                string targetAssemblyPath = Path.GetFileName(DebuggeeInfo.TargetAssemblyPath);
                if (DebuggeeInfo.TargetAssemblyPath == targetAssemblyPath) {
                    // don't use assembly file name as TargetAssemblyPath
                    throw new NetcoreDbgTestCore.ResultNotSuccessException();
                }
                string pwd = DebuggeeInfo.TargetAssemblyPath.Substring(
                                0,
                                DebuggeeInfo.TargetAssemblyPath.Length - targetAssemblyPath.Length);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-environment-cd " + pwd).Class);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-file-exec-and-symbols "
                                                + DebuggeeInfo.CorerunPath).Class);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-exec-arguments "
                                                        + targetAssemblyPath).Class);

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-run").Class);

                Context.WasEntryPointHit();
                Context.Continue();
            });

            Console.WriteLine("Hello World!");

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
