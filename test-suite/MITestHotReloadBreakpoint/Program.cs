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

        public void EnableFuncBreakpoint(string caller_trace, string funcName)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + funcName).Class,
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
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-apply-deltas TestAppHotReload.dll " + targetPath + ".metadata " + targetPath + ".il " + targetPath + ".pdb").Class,
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

namespace MITestHotReloadBreakpoint
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", (Object context) => {
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
                    class Program
                    {
                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadEvalTest1();
                            HotReloadEvalTest1();
                        }
                        static void HotReloadEvalTest1()
                        {
                            Console.WriteLine(""Added string."");
                        }
                    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta1");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta1");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 15);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test1", "bp_test_old_func_resetup", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 15);

                // Setup breakpoints before apply delta.
                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 16);
                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 19);
                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 25);
                Context.EnableFuncBreakpoint(@"__FILE__:__LINE__", "Program.HotReloadEvalTest1");
                Context.EnableFuncBreakpoint(@"__FILE__:__LINE__", "Program.HotReloadEvalTest2");

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;
                namespace TestAppHotReload
                {
                    class Program
                    {
                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadEvalTest1();
                            HotReloadEvalTest1();
                        }
                        static void HotReloadEvalTest1()
                        {
                            Console.WriteLine(""Updated added string."");
                            HotReloadEvalTest2();
                        }
                        static void HotReloadEvalTest2()
                        {
                            Console.WriteLine(""Another added string."");
                            Console.WriteLine(""One more added string."");
                        }
                    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta2");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta2");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test_old_func_resetup", "bp_test_old_line_resetup", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 18); // Program.HotReloadEvalTest1
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test_old_line_resetup", "bp_test_func_added", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 19);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test_func_added", "bp_test_line_added", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 23); // Program.HotReloadEvalTest2
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test_line_added", "bp_test_line_not_changed", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 25);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("bp_test_line_not_changed", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 16);
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
