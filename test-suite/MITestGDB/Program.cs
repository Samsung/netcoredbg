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

                // we don't check exit code here, since Windows and Linux provide different exit code in case of "-gdb-exit" usage
                return true;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

namespace MITestGDB
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");

                var showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class, @"__FILE__:__LINE__");
                Assert.Equal("1", ((MIConst)showResult["value"]).CString, @"__FILE__:__LINE__");

                // NOTE space is only legit delimiter (name-value) for gdb-set (see miengine for more info)
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-gdb-set just-my-code 0").Class,
                             @"__FILE__:__LINE__");

                showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class, @"__FILE__:__LINE__");
                Assert.Equal("0", ((MIConst)showResult["value"]).CString, @"__FILE__:__LINE__");

                // NOTE space is only legit delimiter (name-value) for gdb-set (see miengine for more info)
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-gdb-set just-my-code 1").Class,
                             @"__FILE__:__LINE__");

                showResult = Context.MIDebugger.Request("-gdb-show just-my-code");
                Assert.Equal(MIResultClass.Done, showResult.Class, @"__FILE__:__LINE__");
                Assert.Equal("1", ((MIConst)showResult["value"]).CString, @"__FILE__:__LINE__");

                // Note, reverted sequence here - exit and check, that debuggee process was forced closed.
                Context.DebuggerExit(@"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
            });

            Thread.Sleep(10000);
            Console.WriteLine("Hello World!");
        }
    }
}
