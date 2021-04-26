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

        public void WasManualBreakpointHit(string caller_trace, string bp_fileName, int bp_line)
        {
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

                if (fileName.CString == bp_fileName &&
                    line == bp_line) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
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

namespace MITestBreakpointToModule
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", (Object context) => {
                Context Context = (Context)context;

                var BpResp = Context.MIDebugger.Request("-break-insert -f TestFunc1");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("Hello World!");

            MITestModule1.Class1 testModule1 = new MITestModule1.Class1();
            MITestModule2.Class2 testModule2 = new MITestModule2.Class2();

            testModule1.TestFunc1();
            testModule2.TestFunc1();

            Label.Checkpoint("bp_test1", "bp_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class1.cs", 11);
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class2.cs", 11);

                var BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule1.dll!TestFunc2");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            testModule1.TestFunc2();
            testModule2.TestFunc2();

            Label.Checkpoint("bp_test2", "bp_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class1.cs", 16);

                var BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule3.dll!TestFunc3");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule2.dll!TestFunc4");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            testModule1.TestFunc3();
            testModule2.TestFunc3();

            testModule1.TestFunc4();
            testModule2.TestFunc4();

            Label.Checkpoint("bp_test3", "bp_test4", (Object context) => {
                Context Context = (Context)context;
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class2.cs", 26);

                var BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule2.dll!Class2.cs:31");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });
            
            testModule1.TestFunc5();
            testModule2.TestFunc5();

            Label.Checkpoint("bp_test4", "bp_test5", (Object context) => {
                Context Context = (Context)context;
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class2.cs", 31);

                var BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule1.dll!Class1.cs:31");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            testModule1.TestFunc5();

            Label.Checkpoint("bp_test5", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasManualBreakpointHit(@"__FILE__:__LINE__", "Class1.cs", 31);

                var BpResp = Context.MIDebugger.Request("-break-insert -f .dll!Class1.cs:36");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");
                BpResp = Context.MIDebugger.Request("-break-insert -f MITestModule!Class2.cs:36");
                Assert.Equal(MIResultClass.Done, BpResp.Class, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            testModule1.TestFunc6();
            testModule2.TestFunc6();

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
