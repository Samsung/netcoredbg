using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace NetcoreDbgTest.Script
{
    public static class Context
    {
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

        public static bool IsValidFrame(MIResult frame, string bpName, int level)
        {
            var lbp = (LineBreakpoint)DebuggeeInfo.Breakpoints[bpName];
            var content = (MITuple)frame.Value;
            if (frame.Variable == "frame" &&
                ((MIConst)content["level"]).Int == level &&
                ((MIConst)content["fullname"]).String.Contains("Program.cs") &&
                ((MIConst)content["line"]).Int == lbp.NumLine
            ) {
                return true;
            }

            return false;
        }

        public static bool WasStep(LineBreakpoint lbp)
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                if (reason.CString != "end-stepping-range") {
                    continue;
                }

                var frame = (MITuple)output["frame"];
                var line = ((MIConst)frame["line"]).Int;
                if (lbp.NumLine == line) {
                    return true;
                }
            }

            return false;
        }

        public static void WasExit()
        {
            var records = MIDebugger.Receive();

            foreach (MIOutOfBandRecord record in records) {
                if (!IsStoppedEvent(record)) {
                    continue;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "exited") {
                    continue;
                }

                var exitCode = (MIConst)output["exit-code"];

                if (exitCode.CString == "0") {
                    return;
                } else {
                    throw new NetcoreDbgTestCore.ResultNotSuccessException();
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
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

        public static bool DoStepTo(string lbpName)
        {
            var lbp = (LineBreakpoint)DebuggeeInfo.Breakpoints[lbpName];
            Assert.Equal(MIResultClass.Running, MIDebugger.Request("-exec-next").Class);

            return WasStep(lbp);
        }

        public static MIListElement[] Variables;
        public static System.Func<string, string> GetVarValue = (string name) => {
            var foundVar =
                System.Array.Find(Variables,
                           v => {
                               var tuple = (MITuple)v;
                               return ((MIConst)((MITuple)v)["name"]).CString == name;
                           });

            return ((MIConst)((MITuple)foundVar)["value"]).CString;
        };

        public static MIDebugger MIDebugger = new MIDebugger();
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
            Label.Checkpoint("init", "test_steps", () => {
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("1-file-exec-and-symbols "
                                                        + DebuggeeInfo.CorerunPath).Class);
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("2-exec-arguments "
                                                        + DebuggeeInfo.TargetAssemblyPath).Class);
                Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("3-exec-run").Class);

                Context.WasEntryPointHit();
            });

            P p = new P();                                      Label.Breakpoint("STEP1");

            Label.Checkpoint("test_steps", "try_catch", () => {
                Assert.True(Context.DoStepTo("STEP1"));
                Assert.True(Context.DoStepTo("TRY1"));
                Assert.True(Context.DoStepTo("TRY1"));
                Assert.True(Context.DoStepTo("TRY3"));
                Assert.True(Context.DoStepTo("CATCH1"));
                Assert.True(Context.DoStepTo("CATCH2"));
                Assert.True(Context.DoStepTo("CATCH3"));
                Assert.True(Context.DoStepTo("CATCH4"));
            });

            try {                                               Label.Breakpoint("TRY1");
                try {                                           Label.Breakpoint("TRY2");
                    MyFunction("");                             Label.Breakpoint("TRY3");
                }
                catch (Exception e) {                           Label.Breakpoint("CATCH1"); Label.Breakpoint("CATCH2");
                    int a = 1;                                  Label.Breakpoint("CATCH3");
                }                                               Label.Breakpoint("CATCH4");

                Label.Checkpoint("try_catch", "finish", () => {
                    // Check stack frames location
                    var res = Context.MIDebugger.Request("-stack-list-frames");
                    Assert.Equal(MIResultClass.Done, res.Class);

                    var stack = (MIList)res["stack"];

                    Assert.Equal(3, stack.Count);
                    Assert.True(Context.IsValidFrame((MIResult)stack[0], "CATCH4", 0));
                    Assert.True(Context.IsValidFrame((MIResult)stack[1], "THROW1", 1));
                    Assert.True(Context.IsValidFrame((MIResult)stack[2], "TRY3", 2));

                    //Check local variables
                    res = Context.MIDebugger.Request("-stack-list-variables");
                    Assert.Equal(MIResultClass.Done, res.Class);

                    Context.Variables = ((MIList)res["variables"]).ToArray();

                    Assert.Equal("{System.Exception}", Context.GetVarValue("$exception"));
                    Assert.Equal("{System.Exception}", Context.GetVarValue("e"));
                    Assert.Equal("1", Context.GetVarValue("a"));
                    Assert.Equal("{MITestException.P}", Context.GetVarValue("p"));
                    Assert.Equal("{string[0]}", Context.GetVarValue("args"));

                    // Execute property and check its value
                    res = Context.MIDebugger.Request("-var-create - * \"p.x\"");
                    Assert.Equal(MIResultClass.Done, res.Class);

                    Assert.Equal("111", ((MIConst)res["value"]).CString);
                    Assert.Equal(0, ((MIConst)res["numchild"]).Int);
                    Assert.Equal("int", ((MIConst)res["type"]).CString);
                    Assert.Equal("p.x", ((MIConst)res["exp"]).CString);

                    Assert.Equal(MIResultClass.Running, Context.MIDebugger.Request("-exec-continue").Class);
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

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
            });
        }
    }
}
