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

        public bool IsValidFrame(MIResult frame, string bpName, int level)
        {
            var lbp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];
            var content = (MITuple)frame.Value;
            if (frame.Variable == "frame" &&
                ((MIConst)content["level"]).Int == level &&
                ((MIConst)content["fullname"]).String.Contains("Program.cs") &&
                ((MIConst)content["line"]).Int == lbp.NumLine)
            {
                return true;
            }

            return false;
        }

        public void WasStep(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                if (reason.CString != "end-stepping-range") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var line = ((MIConst)frame["line"]).Int;
                if (bp.NumLine == line) {
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

        public void StepOver(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-next").Class,
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
            GetVarValue = (string name) => {
                var foundVar =
                    System.Array.Find(Variables,
                            v => {
                                var tuple = (MITuple)v;
                                return ((MIConst)((MITuple)v)["name"]).CString == name;
                            });
                return ((MIConst)((MITuple)foundVar)["value"]).CString;
            };
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger { get; private set; }
        public MIListElement[] Variables;
        public System.Func<string, string> GetVarValue;
    }
}

namespace MITestException
{
    class P {
        public int x {
            get { return 111; }
        }
    }

    class Program
    {
        static void MyFunction(string s)
        {
            throw new Exception("test exception");              Label.Breakpoint("THROW1");
        }

        static void MyFunction(int a)
        {
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_steps", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
            });

            P p = new P();                                      Label.Breakpoint("STEP1");

            Label.Checkpoint("test_steps", "try_catch", (Object context) => {
                Context Context = (Context)context;
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "STEP1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "TRY1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "TRY2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "TRY3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "CATCH1");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "CATCH2");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "CATCH3");
                Context.StepOver(@"__FILE__:__LINE__");
                Context.WasStep(@"__FILE__:__LINE__", "CATCH4");
            });

            try {                                               Label.Breakpoint("TRY1");
                try {                                           Label.Breakpoint("TRY2");
                    MyFunction("");                             Label.Breakpoint("TRY3");
                }
                catch (Exception e) {                           Label.Breakpoint("CATCH1"); Label.Breakpoint("CATCH2");
                    int a = 1;                                  Label.Breakpoint("CATCH3");
                }                                               Label.Breakpoint("CATCH4");

                Label.Checkpoint("try_catch", "finish", (Object context) => {
                    Context Context = (Context)context;
                    // Check stack frames location
                    var res = Context.MIDebugger.Request("-stack-list-frames");
                    Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__");

                    var stack = (MIList)res["stack"];

                    Assert.Equal(3, stack.Count, @"__FILE__:__LINE__");
                    Assert.True(Context.IsValidFrame((MIResult)stack[0], "CATCH4", 0), @"__FILE__:__LINE__");
                    Assert.True(Context.IsValidFrame((MIResult)stack[1], "THROW1", 1), @"__FILE__:__LINE__");
                    Assert.True(Context.IsValidFrame((MIResult)stack[2], "TRY3", 2), @"__FILE__:__LINE__");

                    //Check local variables
                    res = Context.MIDebugger.Request("-stack-list-variables");
                    Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__");

                    Context.Variables = ((MIList)res["variables"]).ToArray();

                    Assert.Equal("{System.Exception}", Context.GetVarValue("$exception"), @"__FILE__:__LINE__");
                    Assert.Equal("{System.Exception}", Context.GetVarValue("e"), @"__FILE__:__LINE__");
                    Assert.Equal("1", Context.GetVarValue("a"), @"__FILE__:__LINE__");
                    Assert.Equal("{MITestException.P}", Context.GetVarValue("p"), @"__FILE__:__LINE__");
                    Assert.Equal("{string[0]}", Context.GetVarValue("args"), @"__FILE__:__LINE__");

                    // Execute property and check its value
                    res = Context.MIDebugger.Request("-var-create - * \"p.x\"");
                    Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__");

                    Assert.Equal("111", ((MIConst)res["value"]).CString, @"__FILE__:__LINE__");
                    Assert.Equal(0, ((MIConst)res["numchild"]).Int, @"__FILE__:__LINE__");
                    Assert.Equal("int", ((MIConst)res["type"]).CString, @"__FILE__:__LINE__");
                    Assert.Equal("p.x", ((MIConst)res["exp"]).CString, @"__FILE__:__LINE__");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                int b = 2;                                      Label.Breakpoint("TRY4");

                try {
                    MyFunction("");
                }
                catch (Exception e) {
                    int c = 1;
                    throw new Exception("my exception");
                }
            }
            catch (Exception e) {
                MyFunction(1);
            }

            int d = 1;

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
