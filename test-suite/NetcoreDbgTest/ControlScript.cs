using System;
using System.Runtime;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Collections.Generic;
using System.Text;

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

using NetcoreDbgTest;

namespace NetcoreDbgTestCore
{
    public class ScriptNotBuiltException : Exception
    {
        public ScriptNotBuiltException(EmitResult result)
        {
            Result = result;
        }

        public override string ToString()
        {
            var sb = new StringBuilder();

            IEnumerable<Diagnostic> failures = Result.Diagnostics.Where(diagnostic =>
                diagnostic.IsWarningAsError ||
                diagnostic.Severity == DiagnosticSeverity.Error);

            foreach (Diagnostic diagnostic in failures) {
                sb.Append(String.Format("{0}: {1}\n", diagnostic.Id, diagnostic.GetMessage()));
            }

            return sb.ToString();
        }

        EmitResult Result;
    }

    public class ControlScript
    {
        public ControlScript(string pathToTestFiles, ProtocolType protocolType)
        {
            ControlScriptDummyText =
@"using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;
using NetcoreDbgTest;
using NetcoreDbgTest.Script;
using Newtonsoft.Json;
using NetcoreDbgTest." + protocolType.ToString() + @";

namespace NetcoreDbgTest.Script
{
}

namespace NetcoreDbgTestCore
{
    public class GeneratedScript
    {
        static Context m_Context;

        public static void Setup(ControlInfo controlInfo, DebuggerClient debuggerClient)
        {
            m_Context = new Context(controlInfo, debuggerClient);
        }

        public static void ExecuteCheckPoints() {}

        public static void Invoke(string id, string next_id, Checkpoint checkpoint)
        {
            checkpoint(m_Context);
        }
    }
}";

            // we may have list of files separated by ';' symbol
            string[] pathToFiles = pathToTestFiles.Split(';');
            List<SyntaxTree> trees = new List<SyntaxTree>();

            foreach (string pathToFile in pathToFiles) {
                if (String.IsNullOrEmpty(pathToFile)) {
                    continue;
                }

                // Read file text and replace __FILE__ and __LINE__ markers.
                string[] lines = File.ReadAllLines(pathToFile);
                List<string> list = new List<string>();
                int lineNum = 1;
                foreach (string line in lines)
                {
                    list.Add(line.Replace("__LINE__", lineNum.ToString()));
                    lineNum++;
                }
                string testSource = String.Join("\n", list.ToArray()).Replace("__FILE__", pathToFile);

                SyntaxTree testTree = CSharpSyntaxTree.ParseText(testSource)
                              .WithFilePath(pathToFile);
                trees.Add(testTree);
            }

            TestLabelsInfo = CollectTestLabelsInfo(trees, protocolType);
            ScriptDeclarations = CollectScriptDeclarations(trees);
            SyntaxTree = BuildTree();
            Compilation compilation = CompileTree(SyntaxTree);
            Assembly scriptAssembly = MakeAssembly(compilation);
            generatedScriptClass = scriptAssembly.GetType("NetcoreDbgTestCore.GeneratedScript");
            Breakpoints = TestLabelsInfo.Breakpoints;
        }

        public Dictionary<string, Breakpoint> Breakpoints;

        public SyntaxTree SyntaxTree;

        public void ExecuteCheckPoints(ControlInfo ControlInfo, DebuggerClient DebuggerClient)
        {
            MethodInfo minfo_setup = generatedScriptClass.GetMethod("Setup");
            try {
                minfo_setup.Invoke(null, new object[]{ControlInfo, DebuggerClient});
            } catch (TargetInvocationException e) {
                throw e.InnerException;
            }

            MethodInfo minfo_execute = generatedScriptClass.GetMethod("ExecuteCheckPoints");
            try {
                minfo_execute.Invoke(null, null);
            } catch (TargetInvocationException e) {
                throw e.InnerException;
            }
        }

        TestLabelsInfo CollectTestLabelsInfo(List<SyntaxTree> trees, ProtocolType protocolType)
        {
            var visitor = new TestLabelsInfoCollector(protocolType);

            foreach (SyntaxTree tree in trees) {
                visitor.Visit(tree.GetRoot());
            }

            return visitor.TestLabelsInfo;
        }

        SyntaxList<MemberDeclarationSyntax> CollectScriptDeclarations(List<SyntaxTree> trees)
        {
            var visitor = new ScriptDeclarationsCollector();

            foreach (SyntaxTree tree in trees) {
                visitor.Visit(tree.GetRoot());
            }

            return visitor.ScriptDeclarations;
        }

        SyntaxTree BuildTree()
        {
            var tree = CSharpSyntaxTree.ParseText(ControlScriptDummyText);

            CSharpSyntaxRewriter visitor;

            visitor = new GeneratedScriptInvokesBuilder(TestLabelsInfo);
            tree = ((GeneratedScriptInvokesBuilder) (visitor)).Visit(tree.GetRoot()).SyntaxTree;
            visitor = new GeneratedScriptDeclarationsBuilder(ScriptDeclarations);
            tree = ((GeneratedScriptDeclarationsBuilder) (visitor)).Visit(tree.GetRoot()).SyntaxTree;

            return tree;
        }

        MetadataReference[] GetMetadataReferences()
        {
            var systemPath = Path.GetDirectoryName(typeof(object).Assembly.Location);
            var libList = new List<MetadataReference>();

            string[] libNames = {
                "System.Private.CoreLib.dll",
                "System.Runtime.dll",
                "System.Collections.dll",
                "System.IO.FileSystem.dll",
                "System.Diagnostics.Process.dll",
                "System.ComponentModel.Primitives.dll",
                "System.Runtime.InteropServices.RuntimeInformation.dll",
                "netstandard.dll",
            };

            foreach (var name in libNames) {
                libList.Add(MetadataReference.CreateFromFile(Path.Combine(systemPath, name)));
            }

            libList.Add(MetadataReference.CreateFromFile(typeof(Label).Assembly.Location));
            libList.Add(MetadataReference.CreateFromFile(typeof(Newtonsoft.Json.JsonConvert).Assembly.Location));

            return libList.ToArray();
        }

        Compilation CompileTree(SyntaxTree tree)
        {
            //var CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)
            var compilation = CSharpCompilation.Create(
                "ControlScript",
                new SyntaxTree[] {tree},
                GetMetadataReferences(),
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)
            );

            return compilation;
        }

        Assembly MakeAssembly(Compilation compilation)
        {
            using (var ms = new MemoryStream())
            {
                EmitResult result = compilation.Emit(ms);

                if (!result.Success) {
                    Console.WriteLine("*** Build Error log start ***");
                    foreach (var line in result.Diagnostics) {
                        Console.WriteLine(line);
                    }
                    Console.WriteLine("*** Build Error log end ***");
                    throw new ScriptNotBuiltException(result);
                }

                ms.Seek(0, SeekOrigin.Begin);
                return Assembly.Load(ms.ToArray());
            }
        }

        Type generatedScriptClass = null;
        TestLabelsInfo TestLabelsInfo = null;
        SyntaxList<MemberDeclarationSyntax> ScriptDeclarations;
        string ControlScriptDummyText;
    }

    public class TestLabelsInfo
    {
        public TestLabelsInfo()
        {
            CheckPointInvokes = new Dictionary<string, Tuple<string, InvocationExpressionSyntax>>();
            Breakpoints = new Dictionary<string, Breakpoint>();
        }

        public Dictionary<string, Tuple<string, InvocationExpressionSyntax>> CheckPointInvokes;
        public Dictionary<string, Breakpoint> Breakpoints;
    }

    public class TestLabelsInfoCollector : CSharpSyntaxWalker
    {
        public TestLabelsInfoCollector(ProtocolType protocolType)
        {
            TestLabelsInfo = new TestLabelsInfo();
            TypeClassBP = Type.GetType("NetcoreDbgTest." + protocolType.ToString() + "."
                                       + protocolType.ToString() +"LineBreakpoint");
        }

        public override void VisitInvocationExpression(InvocationExpressionSyntax node)
        {
            string fileName = Path.GetFileName(node.SyntaxTree.FilePath);
            FileLinePositionSpan lineSpan = node.SyntaxTree.GetLineSpan(node.Span);
            int numLine = lineSpan.StartLinePosition.Line + 1;

            switch (node.Expression.ToString()) {
                case "Label.Breakpoint":
                    string name = node.ArgumentList.Arguments[0].Expression.ToString();
                    // "name" -> name
                    name = name.Trim(new char[]{ '\"' });

                    LineBreakpoint lbp = (LineBreakpoint)Activator.CreateInstance(TypeClassBP,
                                                                                  name, fileName, numLine);

                    TestLabelsInfo.Breakpoints.Add(name, lbp);
                    break;
                case "Label.Checkpoint":
                    string id = node.ArgumentList.Arguments[0].Expression.ToString();
                    string next_id = node.ArgumentList.Arguments[1].Expression.ToString();
                    // "id" -> id
                    id = id.Trim(new char[]{ '\"' });
                    next_id = next_id.Trim(new char[]{ '\"' });

                    TestLabelsInfo.CheckPointInvokes.Add(
                        id, new Tuple<string, InvocationExpressionSyntax>(next_id, node));
                    break;
            }
        }

        public TestLabelsInfo TestLabelsInfo;
        Type TypeClassBP;
    }

    public class GeneratedScriptInvokesBuilder : CSharpSyntaxRewriter
    {
        public GeneratedScriptInvokesBuilder(TestLabelsInfo testLabels)
        {
            TestLabelsInfo = testLabels;
        }

        public override SyntaxNode VisitMethodDeclaration(MethodDeclarationSyntax node)
        {
            var controlEnterMember =
                SyntaxFactory.MemberAccessExpression(
                    SyntaxKind.SimpleMemberAccessExpression,
                    SyntaxFactory.IdentifierName("GeneratedScript"),
                    SyntaxFactory.IdentifierName("Invoke")
                );

            List<InvocationExpressionSyntax> invokes = new List<InvocationExpressionSyntax>();

            switch (node.Identifier.ToString()) {
                case "ExecuteCheckPoints":
                    string key = "init";
                    Tuple<string, InvocationExpressionSyntax> Value;
                    do {
                        if (TestLabelsInfo.CheckPointInvokes.TryGetValue(key, out Value)) {
                            invokes.Add(Value.Item2);
                            if (key == "finish") {
                                key = null;
                            } else {
                                key = Value.Item1;
                            }
                        } else {
                            Console.Error.WriteLine("Error! Can't find \"" + key + "\" checkpoint");
                            throw new BrokenTestCheckpointLogic();
                        }
                    } while (!String.IsNullOrEmpty(key));
                    break;
                default:
                    return node;
            }

            List<StatementSyntax> statements = new List<StatementSyntax>();

            foreach (var invoke in invokes) {
                var invokeEnter =
                    SyntaxFactory.InvocationExpression(controlEnterMember)
                    .WithArgumentList(invoke.ArgumentList);

                statements.Add(SyntaxFactory.ExpressionStatement(invokeEnter));
            }

            return node.WithBody(SyntaxFactory.Block(statements.ToArray()));
        }

        TestLabelsInfo TestLabelsInfo;
    }

    public class ScriptDeclarationsCollector : CSharpSyntaxWalker
    {
        public ScriptDeclarationsCollector()
        {
            ScriptDeclarations = new SyntaxList<MemberDeclarationSyntax>(
                new List<MemberDeclarationSyntax>()
            );
        }

        public override void VisitNamespaceDeclaration(NamespaceDeclarationSyntax node)
        {
            if (node.Name.ToString() == "NetcoreDbgTest.Script") {
                ScriptDeclarations = ScriptDeclarations.AddRange(node.Members);
                return;
            }

            foreach (SyntaxNode subNode in node.Members) {
                this.Visit(subNode);
            }
        }

        public SyntaxList<MemberDeclarationSyntax> ScriptDeclarations;
    }

    public class GeneratedScriptDeclarationsBuilder : CSharpSyntaxRewriter
    {
        public GeneratedScriptDeclarationsBuilder(SyntaxList<MemberDeclarationSyntax> declarations)
        {
            ScriptDeclarations = declarations;
        }

        public override SyntaxNode VisitNamespaceDeclaration(NamespaceDeclarationSyntax node)
        {
            if (node.Name.ToString() == "NetcoreDbgTest.Script") {
                return node.AddMembers(ScriptDeclarations.ToArray());
            }

            return node;
        }

        SyntaxList<MemberDeclarationSyntax> ScriptDeclarations;
    }
}
