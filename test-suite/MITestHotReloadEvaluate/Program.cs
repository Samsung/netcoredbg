using System;
using System.IO;
using System.Runtime.InteropServices;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;
using NetcoreDbgTest.GetDeltaApi;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set enable-hot-reload 1").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

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

        public void WasEntryPointHit(string realNamespace, string caller_trace)
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
                if (func.CString == realNamespace + ".Program.Main()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpFileName, int bpNumLine)
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

                if (fileName.CString == bpFileName &&
                    line == bpNumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
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

        public void EnableBreakpoint(string caller_trace, string bpFileName, int bpNumLine)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + bpFileName + ":" + bpNumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public string GetAndCheckValue(string caller_trace, string ExpectedResult, string ExpectedType, string Expression)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", Expression));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(Expression, ((MIConst)res["exp"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedType, ((MIConst)res["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            return ((MIConst)res["name"]).CString;
        }

        public void CheckErrorAtRequest(string caller_trace, string Expression, string errMsgStart)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", Expression));
            Assert.Equal(MIResultClass.Error, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.True(((MIConst)res["msg"]).CString.StartsWith(errMsgStart), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CheckHostRuntimeVersion(string caller_trace)
        {
            Assert.True(GetDeltaApi.CheckRuntimeVersion(), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CheckHostOS(string caller_trace)
        {
            Assert.True(RuntimeInformation.IsOSPlatform(OSPlatform.Linux), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CheckTargetRuntimeVersion(string caller_trace)
        {
            var res = MIDebugger.Request("-var-create - * System.Environment.Version.Major>=6");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n" + caller_trace);
            Assert.Equal("true", ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n" + caller_trace);
        }

        public void StartGenDeltaSession(string caller_trace)
        {
            string projectPath = Path.GetDirectoryName(ControlInfo.SourceFilesPath);
            Assert.True(GetDeltaApi.StartGenDeltaSession(projectPath), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void EndGenDeltaSession(string caller_trace)
        {
            Assert.True(GetDeltaApi.EndGenDeltaSession(), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void SdbPush(string caller_trace, string hostFullPath, string targetPath)
        {
            System.Diagnostics.Process process = new System.Diagnostics.Process();
            process.StartInfo.WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden;
            process.StartInfo.RedirectStandardInput = true;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.CreateNoWindow = true;
            process.StartInfo.UseShellExecute = false;
            
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                process.StartInfo.FileName = "bash";
                process.StartInfo.Arguments = "-c \"sdb push " + hostFullPath + " " + targetPath + "\"";
            }
            else
                throw new Exception("Host OS not supported. " + @"__FILE__:__LINE__"+"\n"+caller_trace);

            process.Start();
            process.WaitForExit();
            Assert.Equal(0, process.ExitCode, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WriteDeltas(string caller_trace, string fileName)
        {
            string hostPath;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                hostPath = Path.Combine(@"/tmp", fileName);
            else
                throw new Exception("Host OS not supported. " + @"__FILE__:__LINE__"+"\n"+caller_trace);

            string targetPath = @"/tmp";
            Assert.True(GetDeltaApi.WriteDeltas(hostPath), @"__FILE__:__LINE__"+"\n"+caller_trace);
            SdbPush(@"__FILE__:__LINE__"+"\n"+caller_trace, hostPath + ".metadata", targetPath);
            SdbPush(@"__FILE__:__LINE__"+"\n"+caller_trace, hostPath + ".il", targetPath);
            SdbPush(@"__FILE__:__LINE__"+"\n"+caller_trace, hostPath + ".pdb", targetPath);
        }

        public void GetDelta(string caller_trace, string source, string sourceFileName)
        {
            Assert.True(GetDeltaApi.GetDeltas(source, sourceFileName, "", false), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void ApplyDeltas(string caller_trace, string fileName)
        {
            string targetPath = Path.Combine(@"/tmp", fileName);
            string targetAssemblyName = Path.GetFileName(ControlInfo.TargetAssemblyPath);
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-apply-deltas " + targetAssemblyName + " " + targetPath + ".metadata " + targetPath + ".il " + targetPath + ".pdb").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
            GetDeltaApi = new GetDeltaApi.GetDeltaApi();
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
        GetDeltaApi.GetDeltaApi GetDeltaApi;
    }
}

namespace MITestHotReloadEvaluate
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "eval_test1", (Object context) => {
                Context Context = (Context)context;
                Context.CheckHostRuntimeVersion(@"__FILE__:__LINE__");
                Context.CheckHostOS(@"__FILE__:__LINE__");
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit("TestAppHotReload", @"__FILE__:__LINE__");
                // Note, target Hot Reload check must be after debuggee process start and stop at entry breakpoint.
                Context.CheckTargetRuntimeVersion(@"__FILE__:__LINE__");
                Context.StartGenDeltaSession(@"__FILE__:__LINE__");

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;
                namespace TestAppHotReload
                {
                    class TestClass
                    {
                        int i = 5;
                    }

                    class Program
                    {
                        static int iStatic = 10;

                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadEvalTest1(555);
                            HotReloadEvalTest1(555);
                        }
                        static void HotReloadEvalTest1(int iArg)
                        {
                            int iLocal = 22;
                            var clLocal = new TestClass();
                            Console.WriteLine(""iArg="" + iArg.ToString());
                            Console.WriteLine(""iLocal="" + iLocal.ToString());
                            Console.WriteLine(""iStatic="" + iStatic.ToString());
                            {
                                int iLocalScope = 33;
                                Console.WriteLine(""iLocalScope="" + iLocalScope.ToString());                // line 33
                            }
                        }                                                                                    // line 35
                    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta1");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta1");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 33);
                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 35);

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("eval_test1", "eval_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 33);

                // Check argument in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "iArg");
                // Check local variable in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "iLocal");
                // Check local variable in scope in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "iLocalScope");
                // Check added static field.
                // Note, it have `0` since static constructor was called before we add this field.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "iStatic");
                // Check field in added class.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "clLocal.i");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("eval_test2", "eval_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 35);

                // Check argument in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "iArg");
                // Check local variable in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "iLocal");
                // Check local variable out of scope in added method.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "iLocalScope", "error");
                // Check added static field.
                // Note, it have `0` since static constructor was called before we add this field.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "iStatic");
                // Check field in added class.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "clLocal.i");
            });

            Label.Checkpoint("eval_test3", "eval_test4", (Object context) => {
                Context Context = (Context)context;

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;
                namespace TestAppHotReload
                {
                    class TestClass
                    {
                        int i = 5;
                    }

                    class Program
                    {
                        static int iStatic = 10;

                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadEvalTest1(555);
                            HotReloadEvalTest1(555);
                        }
                        static void HotReloadEvalTest1(int iArg)
                        {                                                                                    // line 25
                        }
                    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta2");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta2");

                // Check argument in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "iArg");
                // Check local variable in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "iLocal");
                // Check local variable out of scope in added method.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "iLocalScope", "error");
                // Check added static field.
                // Note, it have `0` since static constructor was called before we add this field.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "iStatic");
                // Check field in added class.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "clLocal.i");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 25);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("eval_test4", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 25);

                // Check argument in added method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "iArg");
                // Check local variable in added method.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "iLocal", "error");
                // Check local variable out of scope in added method.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "iLocalScope", "error");
                // Check added static field.
                // Note, it have `0` since static constructor was called before we add this field.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "iStatic");
                // Check field in added class.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "clLocal.i", "error");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.EndGenDeltaSession(@"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
