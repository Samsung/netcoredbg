using System;
using System.Diagnostics;
using System.Text;
using System.IO;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Threading.Tasks.Dataflow;
using Xunit;
using Xunit.Abstractions;

using System.Text.RegularExpressions;
using System.Reflection;

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.CSharp;
using System.Runtime.CompilerServices;
using Microsoft.CodeAnalysis.Scripting;

using System.Threading;

namespace Runner
{
    public class TestRunner
    {
        [Fact]
        public void SimpleSteppingTest() => ExecuteTest();

        [Fact]
        public void ValuesTest() => ExecuteTest();
        public class ProcessInfo
        {
            public ProcessInfo(string command, ITestOutputHelper output)
            {
                this.output = output;
                process = new Process();
                queue = new BufferBlock<string>();

                process.StartInfo.CreateNoWindow = true;
                process.StartInfo.RedirectStandardOutput = true;
                process.StartInfo.RedirectStandardInput = true;
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.Arguments = String.Format("-c \"{0}\"", command);
                process.StartInfo.FileName = "/bin/sh";

                // enable raising events because Process does not raise events by default
                process.EnableRaisingEvents = true;
                // attach the event handler for OutputDataReceived before starting the process
                process.OutputDataReceived += new DataReceivedEventHandler
                (
                    delegate(object sender, DataReceivedEventArgs e)
                    {
                        if (e.Data is null)
                            return;
                        // append the new data to the data already read-in
                        output.WriteLine("> " + e.Data);
                        queue.Post(e.Data);
                    }
                );
                process.Exited += new EventHandler
                (
                    delegate(object sender, EventArgs args)
                    {
                        // This is where you can add some code to be
                        // executed before this program exits.
                        queue.Complete();
                    }
                );

                try
                {
                    process.Start();
                }
                catch (System.ComponentModel.Win32Exception)
                {
                    throw new Exception("Unable to run: " + command);
                }

                process.BeginOutputReadLine();
            }

            public void Close()
            {
                process.StandardInput.Close();
                if (!process.WaitForExit(5))
                {
                    process.CancelOutputRead();
                    process.Close();
                }
                else
                    process.CancelOutputRead();
            }

            public string Receive()
            {
                return queue.ReceiveAsync().Result;
            }

            public MICore.Results Expect(string text, int timeoutSec)
            {
                TimeSpan timeSpan = TimeSpan.FromSeconds(timeoutSec);

                CancellationTokenSource ts = new CancellationTokenSource();

                ts.CancelAfter(timeSpan);
                CancellationToken token = ts.Token;
                token.ThrowIfCancellationRequested();

                try
                {
                    while (true)
                    {
                        Task<string> intputTask = queue.ReceiveAsync();
                        intputTask.Wait(token);
                        string result = intputTask.Result;
                        if (result.StartsWith(text))
                        {
                            var parser = new MICore.MIResults();
                            return parser.ParseCommandOutput(result);
                        }
                    }
                }
                catch (AggregateException e)
                {
                    foreach (var v in e.InnerExceptions)
                        output.WriteLine(e.Message + " " + v.Message);
                }
                catch (OperationCanceledException)
                {
                }
                finally
                {
                    ts.Dispose();
                }
                throw new Exception($"Expected '{text}' in {timeSpan}");
            }

            public void Send(string s)
            {
                process.StandardInput.WriteLine(s);
                output.WriteLine("< " + s);
            }

            public Process process;
            private readonly ITestOutputHelper output;
            public BufferBlock<string> queue;
        }

        public class TestCaseGlobals
        {
            public readonly Dictionary<string, int> Lines;
            private ProcessInfo processInfo;
            public TestCaseGlobals(
                ProcessInfo processInfo,
                Dictionary<string, int> lines,
                string testSource,
                string testBin,
                ITestOutputHelper output)
            {
                this.processInfo = processInfo;
                this.Lines = lines;
                this.TestSource = testSource;
                this.TestBin = testBin;
                this.Output = output;
            }
            public int GetCurrentLine([CallerLineNumber] int line = 0) { return line; }

            public void Send(string s) => processInfo.Send(s);
            public MICore.Results Expect(string s, int timeoutSec = 10) => processInfo.Expect(s, timeoutSec);
            public readonly string TestSource;
            public readonly string TestBin;
            public readonly ITestOutputHelper Output;
        }

        // Fill 'Tags' dictionary with tags lines
        Dictionary<string, int> CollectTags(string srcName)
        {
            Dictionary<string, int> Tags = new Dictionary<string, int>();
            int lineCounter = 0;
            string[] separators = new string[] {"//"};
            string pattern = @".*@([^@]+)@.*";
            Regex reg = new Regex (pattern);

            foreach (string line in File.ReadLines(srcName))
            {
                lineCounter++;

                Match match = reg.Match(line);

                if (!match.Success)
                    continue;

                string key = match.Groups[1].ToString().Trim();
                if (Tags.ContainsKey(key))
                    throw new Exception(String.Format("Tag '{0}' is present more than once in file '{1}'", key, srcName));
                Tags[key] = lineCounter;
            }

            return Tags;
        }

        private class CommentCollector : CSharpSyntaxRewriter
        {
            private StringBuilder allComments = new StringBuilder();
            private int lineCount = 0;
            public override SyntaxTrivia VisitTrivia(SyntaxTrivia trivia)
            {
                if (!trivia.IsKind(SyntaxKind.SingleLineCommentTrivia) && !trivia.IsKind(SyntaxKind.MultiLineCommentTrivia))
                    return trivia;

                var lineSpan = trivia.GetLocation().GetLineSpan();

                while (lineCount < lineSpan.StartLinePosition.Line)
                {
                    allComments.AppendLine();
                    lineCount++;
                }
                string comment = trivia.ToString();
                if (trivia.IsKind(SyntaxKind.SingleLineCommentTrivia))
                    comment = comment.Substring(2);
                else if (trivia.IsKind(SyntaxKind.MultiLineCommentTrivia))
                    comment = comment.Substring(2, comment.Length - 4);

                allComments.Append(comment);
                lineCount = lineSpan.EndLinePosition.Line;

                return trivia;
            }
            public string Text { get => allComments.ToString(); }
        }

        class TestData
        {
            public TestData(string dllPath, string srcFilePath)
            {
                this.dllPath = dllPath;
                this.srcFilePath = srcFilePath;
            }
            public string srcFilePath { get; }
            public string dllPath { get; }

        }

        private readonly ITestOutputHelper output;
        private string debuggerCommand;
        private Dictionary<string, TestData> allTests;
        public TestRunner(ITestOutputHelper output)
        {
            this.output = output;

            // Sneaky way to get assembly path, which works even if call
            // current constructor with reflection
            string codeBase = Assembly.GetExecutingAssembly().CodeBase;
            UriBuilder uri = new UriBuilder(codeBase);
            string path = Uri.UnescapeDataString(uri.Path);
            var d = new DirectoryInfo(Path.GetDirectoryName(path));

            var pipe = Environment.GetEnvironmentVariable("PIPE");
            if (pipe != null)
            {
                this.debuggerCommand = pipe;
            }
            else
            {
                this.debuggerCommand = Path.Combine(d.Parent.Parent.Parent.Parent.Parent.FullName, "bin", "netcoredbg");
            }

            var testDir = Environment.GetEnvironmentVariable("TESTDIR");

            // Find all dlls
            var baseDir = d.Parent.Parent.Parent.Parent;
            var files = Directory.GetFiles(baseDir.FullName, "*.dll", SearchOption.AllDirectories);

            allTests = new Dictionary<string, TestData>();

            foreach (var dll in files)
            {
                string testName = dll.Substring(0, dll.Length - 4);
                var configName = new FileInfo(testName + ".runtimeconfig.json");
                if (configName.Exists)
                {
                    var csFiles = Directory.GetFiles(configName.Directory.Parent.Parent.Parent.FullName, "*.cs");

                    string testDll = testDir != null ? Path.Combine(testDir, Path.GetFileName(dll)) : dll;

                    allTests[Path.GetFileName(testName)] = new TestData(testDll, csFiles[0]);
                }
            }
        }

        private void ExecuteTest([CallerMemberName] string name = null)
        {
            var data = allTests[name];

            var lines = CollectTags(data.srcFilePath);

            var tree = CSharpSyntaxTree.ParseText(File.ReadAllText(data.srcFilePath))
                                       .WithFilePath(data.srcFilePath);

            var cc = new CommentCollector();
            cc.Visit(tree.GetRoot());

            output.WriteLine("------ Test script ------");
            output.WriteLine(cc.Text);
            output.WriteLine("-------------------------");

            var script = CSharpScript.Create(
                cc.Text,
                ScriptOptions.Default.WithReferences(typeof(object).Assembly)
                                     .WithReferences(typeof(Xunit.Assert).Assembly)
                                     .WithImports("System")
                                     .WithImports("Xunit"),
                globalsType: typeof(TestCaseGlobals)
            );
            script.Compile();

            ProcessInfo processInfo = new ProcessInfo(debuggerCommand, output);

            // Globals, to use inside test case
            TestCaseGlobals globals = new TestCaseGlobals(
                processInfo,
                lines,
                data.srcFilePath,
                data.dllPath,
                output
            );

            script.RunAsync(globals).Wait();

            // Finish process
            processInfo.Close();
        }
    }
}
