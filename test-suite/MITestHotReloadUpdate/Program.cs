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

        public void WasBreakHit(string caller_trace, string bpFileName, int bpNumLine)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                var signal_name = (MIConst)output["signal-name"];

                if (reason.CString != "signal-received" &&
                    signal_name.CString != "SIGINT") {
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
                process.StartInfo.Arguments = "-c \"" + ControlInfo.SDB + " push " + hostFullPath + " " + targetPath + "\"";
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
            SdbPush(@"__FILE__:__LINE__"+"\n"+caller_trace, hostPath + ".bin", targetPath);
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
                         MIDebugger.Request("-apply-deltas " + targetAssemblyName + " " + targetPath + ".metadata " + targetPath + ".il " + targetPath + ".pdb " + targetPath + ".bin").Class,
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

namespace MITestHotReloadUpdate
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_update1", (Object context) => {
                Context Context = (Context)context;
                Context.CheckHostRuntimeVersion(@"__FILE__:__LINE__");
                Context.CheckHostOS(@"__FILE__:__LINE__");
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit("TestAppHotReloadUpdate", @"__FILE__:__LINE__");
                // Note, target Hot Reload check must be after debuggee process start and stop at entry breakpoint.
                Context.CheckTargetRuntimeVersion(@"__FILE__:__LINE__");
                Context.StartGenDeltaSession(@"__FILE__:__LINE__");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "TestAppHotReloadUpdate.Program.i_test6");

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;using System.Diagnostics;using System.Threading;using System.Reflection.Metadata;[assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload.HotReloadNested))]
                namespace TestAppHotReloadUpdate
                {
                    public class Program
                    {
                        static public int i_test1 = 0;
                        static public int i_test2 = 0;
                        static public int i_test3 = 0;
                        static public int i_test4 = 0;
                        static public int i_test5 = 0;
                        static public int i_test6 = 0;
                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World! Main updated."");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadBreakpointTest1();                                                      // line 20
                            System.Threading.Thread.Sleep(1000);
                            HotReloadBreakpointTest1();
                            System.Threading.Thread.Sleep(10000);
                            if (i_test1 == 1000 && i_test2 == 1001 &&
                                i_test3 == 2000 && i_test4 == 2001 &&
                                i_test5 == 3000 && i_test6 == 3001)
                                exit_correct();
                        }
                        static void HotReloadBreakpointTest1()
                        {
                            Console.WriteLine(""Added string."");
                        }
                        static void exit_correct()
                        {
                            Debugger.Break();
                        }

        internal static class HotReload
        {
            public static void ClearCache(Type[]? changedTypes)
            {
                Console.WriteLine(""ClearCache2"");
                TestAppHotReloadUpdate.Program.i_test1 = 10;
            }

            public static void UpdateApplication(Type[]? changedTypes)
            {
                Console.WriteLine(""UpdateApplication2"");
                TestAppHotReloadUpdate.Program.i_test2 = TestAppHotReloadUpdate.Program.i_test1 + 1;
            }
            internal static class HotReloadNested
            {
                public static void ClearCache(Type[]? changedTypes)
                {
                    Console.WriteLine(""ClearCache3"");
                    TestAppHotReloadUpdate.Program.i_test3 = 20;
                }

                public static void UpdateApplication(Type[]? changedTypes)
                {
                    Console.WriteLine(""UpdateApplication3"");
                    TestAppHotReloadUpdate.Program.i_test4 = TestAppHotReloadUpdate.Program.i_test3 + 1;
                }
            }
        }
                    }
    internal static class HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
    {
        public static void ClearCache(Type[]? changedTypes)
        {
            Console.WriteLine(""ClearCache1"");
            TestAppHotReloadUpdate.Program.i_test5 = 30;
        }

        public static void UpdateApplication(Type[]? changedTypes)
        {
            Console.WriteLine(""UpdateApplication1"");
            TestAppHotReloadUpdate.Program.i_test6 = TestAppHotReloadUpdate.Program.i_test5 + 1;
        }
    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta1");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta1");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 20);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("test_update1", "test_update2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 20);

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "TestAppHotReloadUpdate.Program.i_test1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "TestAppHotReloadUpdate.Program.i_test2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "20", "int", "TestAppHotReloadUpdate.Program.i_test3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "21", "int", "TestAppHotReloadUpdate.Program.i_test4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "30", "int", "TestAppHotReloadUpdate.Program.i_test5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "31", "int", "TestAppHotReloadUpdate.Program.i_test6");

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;using System.Diagnostics;using System.Threading;using System.Reflection.Metadata;[assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload.HotReloadNested))]
                namespace TestAppHotReloadUpdate
                {
                    public class Program
                    {
                        static public int i_test1 = 0;
                        static public int i_test2 = 0;
                        static public int i_test3 = 0;
                        static public int i_test4 = 0;
                        static public int i_test5 = 0;
                        static public int i_test6 = 0;
                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadBreakpointTest1();
                            System.Threading.Thread.Sleep(1000);
                            HotReloadBreakpointTest1();                                                      // line 21
                            System.Threading.Thread.Sleep(10000);
                            if (i_test1 == 1000 && i_test2 == 1001 &&
                                i_test3 == 2000 && i_test4 == 2001 &&
                                i_test5 == 3000 && i_test6 == 3001)
                                exit_correct();
                        }
                        static void HotReloadBreakpointTest1()
                        {
                            Console.WriteLine(""Updated added string."");
                        }
                        static void exit_correct()
                        {
                            Debugger.Break();
                        }

        internal static class HotReload
        {
            public static void ClearCache(Type[]? changedTypes)
            {
                if (changedTypes == null || changedTypes == Type.EmptyTypes)
                    return;
                Console.WriteLine(""ClearCache2"");
                TestAppHotReloadUpdate.Program.i_test1 = 100;
            }

            public static void UpdateApplication(Type[]? changedTypes)
            {
                if (changedTypes == null)
                    return;
                bool found = false;
                foreach (var type in changedTypes)
                {
                    if (found = (type == Type.GetType(""TestAppHotReloadUpdate.Program+HotReload"")))
                        break;
                }
                if (!found)
                    return;
                Console.WriteLine(""UpdateApplication2"");
                TestAppHotReloadUpdate.Program.i_test2 = TestAppHotReloadUpdate.Program.i_test1 + 1;
            }
            internal static class HotReloadNested
            {
                public static void ClearCache(Type[]? changedTypes)
                {
                    if (changedTypes == null || changedTypes == Type.EmptyTypes)
                        return;
                    Console.WriteLine(""ClearCache3"");
                    TestAppHotReloadUpdate.Program.i_test3 = 200;
                }

                public static void UpdateApplication(Type[]? changedTypes)
                {
                    if (changedTypes == null)
                        return;
                    bool found = false;
                    foreach (var type in changedTypes)
                    {
                        if (found = (type == Type.GetType(""TestAppHotReloadUpdate.Program+HotReload+HotReloadNested"")))
                            break;
                    }
                    if (!found)
                        return;
                    Console.WriteLine(""UpdateApplication3"");
                    TestAppHotReloadUpdate.Program.i_test4 = TestAppHotReloadUpdate.Program.i_test3 + 1;
                }
            }
        }
                    }
    internal static class HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
    {
        public static void ClearCache(Type[]? changedTypes)
        {
            if (changedTypes == null || changedTypes == Type.EmptyTypes)
                return;
            Console.WriteLine(""ClearCache1"");
            TestAppHotReloadUpdate.Program.i_test5 = 300;
        }

        public static void UpdateApplication(Type[]? changedTypes)
        {
            if (changedTypes == null)
                return;
            bool found = false;
            foreach (var type in changedTypes)
            {
                if (found = (type == Type.GetType(""TestAppHotReloadUpdate.HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111"")))
                    break;
            }
            if (!found)
                return;
            Console.WriteLine(""UpdateApplication1"");
            TestAppHotReloadUpdate.Program.i_test6 = TestAppHotReloadUpdate.Program.i_test5 + 1;
        }
    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta2");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta2");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", @"Program.cs", 21);
                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("test_update2", "test_update3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", @"Program.cs", 21);

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "TestAppHotReloadUpdate.Program.i_test1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "101", "int", "TestAppHotReloadUpdate.Program.i_test2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "200", "int", "TestAppHotReloadUpdate.Program.i_test3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "201", "int", "TestAppHotReloadUpdate.Program.i_test4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "300", "int", "TestAppHotReloadUpdate.Program.i_test5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "301", "int", "TestAppHotReloadUpdate.Program.i_test6");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("test_update3", "test_update4", (Object context) => {
                Context Context = (Context)context;

                System.Threading.Thread.Sleep(2000);

                Context.GetDelta(@"__FILE__:__LINE__",
                @"using System;using System.Diagnostics;using System.Threading;using System.Reflection.Metadata;[assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload.HotReloadNested))]
                namespace TestAppHotReloadUpdate
                {
                    public class Program
                    {
                        static public int i_test1 = 0;
                        static public int i_test2 = 0;
                        static public int i_test3 = 0;
                        static public int i_test4 = 0;
                        static public int i_test5 = 0;
                        static public int i_test6 = 0;
                        static void Main(string[] args)
                        {
                            Console.WriteLine(""Hello World!"");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine(""Updated string."");
                            HotReloadBreakpointTest1();
                            System.Threading.Thread.Sleep(1000);
                            HotReloadBreakpointTest1();
                            if (i_test1 == 1000 && i_test2 == 1001 &&
                                i_test3 == 2000 && i_test4 == 2001 &&
                                i_test5 == 3000 && i_test6 == 3001)
                                exit_correct();
                        }
                        static void HotReloadBreakpointTest1()
                        {
                            Console.WriteLine(""Updated added string."");
                        }
                        static void HotReloadBreakpointTest2()
                        {
                        }
                        static void exit_correct()
                        {
                            Debugger.Break();                                                      // line 37
                        }

        internal static class HotReload
        {
            public static void ClearCache(Type[]? changedTypes)
            {
                Console.WriteLine(""ClearCache2"");
                TestAppHotReloadUpdate.Program.i_test1 = 1000;
            }

            public static void UpdateApplication(Type[]? changedTypes)
            {
                Console.WriteLine(""UpdateApplication2"");
                TestAppHotReloadUpdate.Program.i_test2 = TestAppHotReloadUpdate.Program.i_test1 + 1;
            }
            internal static class HotReloadNested
            {
                public static void ClearCache(Type[]? changedTypes)
                {
                    Console.WriteLine(""ClearCache3"");
                    TestAppHotReloadUpdate.Program.i_test3 = 2000;
                }

                public static void UpdateApplication(Type[]? changedTypes)
                {
                    Console.WriteLine(""UpdateApplication3"");
                    TestAppHotReloadUpdate.Program.i_test4 = TestAppHotReloadUpdate.Program.i_test3 + 1;
                }
            }
        }
                    }
    internal static class HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
    {
        public static void ClearCache(Type[]? changedTypes)
        {
            Console.WriteLine(""ClearCache1"");
            TestAppHotReloadUpdate.Program.i_test5 = 3000;
        }

        public static void UpdateApplication(Type[]? changedTypes)
        {
            Console.WriteLine(""UpdateApplication1"");
            TestAppHotReloadUpdate.Program.i_test6 = TestAppHotReloadUpdate.Program.i_test5 + 1;
        }
    }
                }", @"Program.cs");
                Context.WriteDeltas(@"__FILE__:__LINE__", "tmp_delta3");
                Context.ApplyDeltas(@"__FILE__:__LINE__", "tmp_delta3");
            });

            Label.Checkpoint("test_update4", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakHit(@"__FILE__:__LINE__", @"Program.cs", 37);
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
