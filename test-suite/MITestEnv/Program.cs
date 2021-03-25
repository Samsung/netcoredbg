using System;
using System.IO;
using System.Threading;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
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
    }
}

namespace MITestEnv
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", (Object context) => {
                Context Context = (Context)context;
                string targetAssemblyPath = Path.GetFileName(Context.ControlInfo.TargetAssemblyPath);
                // don't use assembly file name as TargetAssemblyPath
                Assert.NotEqual(Context.ControlInfo.TargetAssemblyPath, targetAssemblyPath, @"__FILE__:__LINE__");
                string pwd = Context.ControlInfo.TargetAssemblyPath.Substring(
                                0,
                                Context.ControlInfo.TargetAssemblyPath.Length - targetAssemblyPath.Length);

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-environment-cd " + pwd).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-file-exec-and-symbols " + Context.ControlInfo.CorerunPath).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-exec-arguments " + targetAssemblyPath).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-run").Class, @"__FILE__:__LINE__");

                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("Hello World!");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
