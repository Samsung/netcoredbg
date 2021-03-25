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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
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
    }
}

namespace MITestEvalNotEnglish
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "eval_test", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK");
                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("영어 출력이 아닌 테스트.");
            Console.WriteLine("测试非英语输出。");

            int 당신 = 1;
            당신++;                                                      Label.Breakpoint("BREAK");

            Label.Checkpoint("eval_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK");

                var notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "你"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class, @"__FILE__:__LINE__");

                notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "你 + 1"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class, @"__FILE__:__LINE__");


                var notEnglishVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "당신"));
                Assert.Equal(MIResultClass.Done, notEnglishVariable.Class, @"__FILE__:__LINE__");
                Assert.Equal("1", ((MIConst)notEnglishVariable["value"]).CString, @"__FILE__:__LINE__");

                notEnglishVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "당신 + 11"));
                Assert.Equal(MIResultClass.Done, notEnglishVariable.Class, @"__FILE__:__LINE__");
                Assert.Equal("12", ((MIConst)notEnglishVariable["value"]).CString, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
