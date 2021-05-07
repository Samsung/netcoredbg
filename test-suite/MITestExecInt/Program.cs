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

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

namespace MITestExecInt
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("1-exec-continue").Class, @"__FILE__:__LINE__");
                // In case of CoreCLR Debug/Checked build must pass all asserts.
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("2-exec-continue").Class, @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("3-exec-interrupt").Class, @"__FILE__:__LINE__");
                // In case of CoreCLR Debug/Checked build must pass all asserts.
                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("4-exec-interrupt").Class, @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("5-exec-continue").Class, @"__FILE__:__LINE__");
                // In case of CoreCLR Debug/Checked build must pass all asserts.
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("6-exec-continue").Class, @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done, Context.MIDebugger.Request("7-exec-abort").Class, @"__FILE__:__LINE__");

                Context.DebuggerExit(@"__FILE__:__LINE__");
            });

            Thread.Sleep(10000);
            Console.WriteLine("Hello World!");
        }
    }
}
