using System;
using System.IO;
using Xunit;

using LocalDebugger;
using NetcoreDbgTestCore;
using NetcoreDbgTestCore.MI;
using NetcoreDbgTestCore.VSCode;

namespace XUnitTests
{
    public class LocalTest : IDisposable
    {
        public LocalTest()
        {
            DebuggerClient = null;
            LocalDebugger = null;
        }

        [Theory]
        [InlineData("MIExampleTest", "Program.cs;Program_second_source.cs")]
        [InlineData("MITestBreakpoint", "Program.cs")]
        [InlineData("MITestExpression", "Program.cs")]
        [InlineData("MITestSetValue", "Program.cs")]
        [InlineData("MITestStepping", "Program.cs")]
        [InlineData("MITestVarObject", "Program.cs")]
        [InlineData("MITestException", "Program.cs")]
        [InlineData("MITestLambda", "Program.cs")]
        [InlineData("MITestEnv", "Program.cs")]
        [InlineData("MITestGDB", "Program.cs")]
        [InlineData("MITestExecFinish", "Program.cs")]
        [InlineData("MITestExecAbort", "Program.cs")]
        [InlineData("MITestExecInt", "Program.cs")]
        [InlineData("MITestHandshake", "Program.cs")]
        [InlineData("MITestTarget", "Program.cs")]
        [InlineData("MITestExceptionBreakpoint", "Program.cs")]
        [InlineData("MITestExitCode", "Program.cs")]
        [InlineData("MITestEvalNotEnglish", "Program.cs")]
        [InlineData("MITest中文目录", "中文文件名.cs")]
        [InlineData("MITestSrcBreakpointResolve", "Program.cs;folder/Program.cs")]
        [InlineData("MITestEnum", "Program.cs")]
        [InlineData("MITestBreak", "Program.cs")]
        [InlineData("MITestAsyncStepping", "Program.cs")]
        [InlineData("VSCodeExampleTest", "Program.cs")]
        [InlineData("VSCodeTestBreakpoint", "Program.cs")]
        [InlineData("VSCodeTestFuncBreak", "Program.cs")]
        [InlineData("VSCodeTestAttach", "Program.cs")]
        [InlineData("VSCodeTestPause", "Program.cs")]
        [InlineData("VSCodeTestDisconnect", "Program.cs")]
        [InlineData("VSCodeTestThreads", "Program.cs")]
        [InlineData("VSCodeTestVariables", "Program.cs")]
        [InlineData("VSCodeTestEvaluate", "Program.cs")]
        [InlineData("VSCodeTestStepping", "Program.cs")]
        [InlineData("VSCodeTestEnv", "Program.cs")]
        [InlineData("VSCodeTestExitCode", "Program.cs")]
        [InlineData("VSCodeTestEvalNotEnglish", "Program.cs")]
        [InlineData("VSCodeTest中文目录", "中文文件名.cs")]
        [InlineData("VSCodeTestSrcBreakpointResolve", "Program.cs;folder/Program.cs")]
        [InlineData("VSCodeTestEnum", "Program.cs")]
        [InlineData("VSCodeTestAsyncStepping", "Program.cs")]
        [InlineData("VSCodeTestBreak", "Program.cs")]
        public void Run(string testCaseName, string testCourceList)
        {
            // Explicit encoding setup, since system console encoding could be not utf8 (Windows OS).
            // Note, netcoredbg aimed to interact with utf8 encoding usage only for all protocols.
            Console.OutputEncoding = System.Text.Encoding.UTF8;
            Console.InputEncoding = System.Text.Encoding.UTF8;

            string testSuiteRoot = Path.GetFullPath(
                Path.Combine(Directory.GetCurrentDirectory(), "../../../..")
            );

            var Env = new NetcoreDbgTestCore.Environment();
            Env.TestName = testCaseName;

            string[] testFileArray = testCourceList.Split(";");
            foreach (var FileName in testFileArray) {
                Env.SourceFilesPath += Path.Combine(testSuiteRoot, testCaseName, FileName + ";");
            }
            Env.SourceFilesPath = Env.SourceFilesPath.Remove(Env.SourceFilesPath.Length - 1);

            Env.TargetAssemblyPath = Path.Combine(testSuiteRoot,
                testCaseName + "/bin/Debug/netcoreapp3.1/",
                testCaseName + ".dll");
            string fullDebuggerPath = Path.GetFullPath(Path.Combine(testSuiteRoot, DebuggerPath));

            if (testCaseName.StartsWith("MI")) {
                LocalDebugger = new LocalDebuggerProcess(fullDebuggerPath, @" --interpreter=mi");
                LocalDebugger.Start();
                DebuggerClient = new MILocalDebuggerClient(LocalDebugger.Input,
                                                           LocalDebugger.Output);
            } else if (testCaseName.StartsWith("VSCode")) {
                LocalDebugger = new LocalDebuggerProcess(fullDebuggerPath, @" --interpreter=vscode");
                LocalDebugger.Start();
                DebuggerClient = new VSCodeLocalDebuggerClient(LocalDebugger.Input,
                                                               LocalDebugger.Output);
            } else {
                throw new System.Exception();
            }

            Xunit.Assert.True(DebuggerClient.DoHandshake(5000));

            var Script = new DebuggeeScript(Env.SourceFilesPath, DebuggerClient.Protocol);

            Debuggee.Run(Script, DebuggerClient, Env);
        }

        public void Dispose()
        {
            if (DebuggerClient != null) {
                DebuggerClient.Close();
            }
            if (LocalDebugger != null) {
                // we may exit debugger by "gdb-exit" call in command script
                try
                {
                    LocalDebugger.Close();
                }
                // "No such process" exception at System.Diagnostics.Process.Kill()
                catch (System.ComponentModel.Win32Exception) {}
            }
        }

        private LocalDebuggerProcess LocalDebugger;
        private DebuggerClient DebuggerClient;
        private static string DebuggerPath = "../bin/netcoredbg";
    }
}
