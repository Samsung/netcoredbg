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
using System.Linq;
using System.Reflection;

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using System.Runtime.CompilerServices;
using Microsoft.CodeAnalysis.Scripting;

using System.Threading;

namespace Runner
{
    public class Labeled<T>
    {
        public Labeled(T data, string label)
        {
            Data = data;
            Label = label;
        }

        public T Data { get; }
        public string Label { get; }

        public override string ToString()
        {
            return Label;
        }
    }

    public static class Labeledextensions
    {
        public static Labeled<T> Labeled<T>
                (this T source, string label)=>new Labeled<T>( source, label );
    }

    public partial class TestRunner
    {
        public static IEnumerable<object[]> Data()
        {
            object[] make
            (string binName,
             string srcName,
             string label)
            {
                return new object []
                { ( binName:binName
                , srcName:srcName
                )
                .Labeled(label)
                };
            }
            var data = new List<object[]>();

            // Sneaky way to get assembly path, which works even if call
            // current constructor with reflection
            string codeBase = Assembly.GetExecutingAssembly().CodeBase;
            UriBuilder uri = new UriBuilder(codeBase);
            string path = Uri.UnescapeDataString(uri.Path);
            var d = new DirectoryInfo(Path.GetDirectoryName(path));

            // Get path to runner binaries
            path = Path.Combine(d.Parent.Parent.Parent.Parent.FullName, "runner");
            var files = Directory.GetFiles(path, "*.dll", SearchOption.AllDirectories);
            var depsJson = new FileInfo(files[0].Substring(0, files[0].Length - 4) + ".deps.json");
            var runnerPath = depsJson.Directory.Parent.Parent.Parent.FullName;

            // Find all dlls
            var baseDir = d.Parent.Parent.Parent.Parent;
            files = Directory.GetFiles(baseDir.FullName, "*.dll", SearchOption.AllDirectories);

            foreach (var dll in files)
            {
                string testName = dll.Substring(0, dll.Length - 4);
                depsJson = new FileInfo(testName + ".deps.json");
                var dllDir = depsJson.Directory.Parent.Parent.Parent.FullName;
                // Do not use as test cases runner and launcher files
                if (depsJson.Exists &&
                    !dllDir.Equals(runnerPath, StringComparison.CurrentCultureIgnoreCase))
                {
                    var csFiles = Directory.GetFiles(depsJson.Directory.Parent.Parent.Parent.FullName, "*.cs");
                    data.Add(make(dll, csFiles[0], testName.Split('/').Last()));
                }
            }

            return data;
        }

        public class ProcessInfo
        {
            public ProcessInfo(string binName, ITestOutputHelper output)
            {
                this.output = output;
                process = new Process();
                queue = new BufferBlock<string>();

                process.StartInfo.CreateNoWindow = true;
                process.StartInfo.RedirectStandardOutput = true;
                process.StartInfo.RedirectStandardInput = true;
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.Arguments = "";
                process.StartInfo.FileName = binName;

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
                    throw new Exception("Unable to run process: " + binName);
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

            public MICore.Results Expect(string text, int timeoutSec = 10)
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
            public MICore.Results Expect(string s) => processInfo.Expect(s);
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
                    throw new Exception(String.Format("Tag '{0}' presented more than once in file '{1}'", key, srcName));
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

        private readonly ITestOutputHelper output;
        private string debugger;
        public TestRunner(ITestOutputHelper output)
        {
            this.output = output;

            // Sneaky way to get assembly path, which works even if call
            // current constructor with reflection
            string codeBase = Assembly.GetExecutingAssembly().CodeBase;
            UriBuilder uri = new UriBuilder(codeBase);
            string path = Uri.UnescapeDataString(uri.Path);
            var d = new DirectoryInfo(Path.GetDirectoryName(path));

            // Get path to runner binaries
            this.debugger = Path.Combine(d.Parent.Parent.Parent.Parent.Parent.FullName, "bin", "netcoredbg");
        }

        [Theory]
        [MemberData(nameof(Data))]
        public void ExecuteTest(Labeled<(string binName, string srcName)> t)
        {
            var lines = CollectTags(t.Data.srcName);

            var tree = CSharpSyntaxTree.ParseText(File.ReadAllText(t.Data.srcName))
                                       .WithFilePath(t.Data.srcName);

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

            ProcessInfo processInfo = new ProcessInfo(debugger, output);

            // Globals, to use inside test case
            TestCaseGlobals globals = new TestCaseGlobals(
                processInfo,
                lines,
                t.Data.srcName,
                t.Data.binName,
                output
            );

            script.RunAsync(globals).Wait();

            // Finish process
            processInfo.Close();
        }
    }
}
