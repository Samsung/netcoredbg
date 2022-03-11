using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;
using Microsoft.CodeAnalysis.MSBuild;
using Microsoft.Build.Locator;

namespace NetcoreDbgTest.GetDeltaApi
{
    public class GetDeltaApiResult : Tuple<bool, string>
    {
        public GetDeltaApiResult(bool Success, string ResponseStr)
            :base(Success, ResponseStr)
        {
        }

        public bool Success { get{ return this.Item1; } }
        public string ResponseStr { get{ return this.Item2; } }
    }
    public class GetDeltaApi
    {
        private static Solution solution;
        private static WatchHotReloadService watchHotReloadService;
        private static Update delta;

        /// <summary>
        /// Check .Net runtime version on test system
        /// </summary>
        public bool CheckRuntimeVersion()
        {
            //Calculation of deltas should only be run for version 6.0 or higher
            if (Environment.Version.Major<6)
                return false;
            #if !NET6_0_OR_GREATER
                return false;
            #endif
            return true;
        }

        /// <summary>
        /// Get path to project
        /// </summary>
        /// <param name="projectPath">path to project</param>
        /// <returns></returns>
        /// <exception cref="FileLoadException">More than one project</exception>
        public bool StartGenDeltaSession(string projectPath)
        {
            try
            {
                MSBuildLocator.RegisterDefaults();
                var workspace = MSBuildWorkspace.Create();
                var path = GetMSBuildProjPath(projectPath);
                var project = workspace.OpenProjectAsync(path, cancellationToken: CancellationToken.None).Result;

                solution = project.Solution;
                watchHotReloadService = new WatchHotReloadService(solution.Workspace.Services);
                watchHotReloadService.StartSessionAsync(solution, CancellationToken.None).Wait();
            }
            catch(Exception exception)
            {
                return false;
            }
            return true;
        }

        /// <summary>
        /// Get path to project
        /// </summary>
        /// <param name="projectDirectory">path to project</param>
        /// <returns></returns>
        /// <exception cref="FileLoadException">More than one project</exception>
        private static string GetMSBuildProjPath(string projectDirectory)
        {
            IEnumerable<string> projectFiles = Directory.GetFiles(projectDirectory, "*.csproj");

            if (projectFiles.Count() == 0)
            {
                throw new FileLoadException("File not found");
            }
            else if (projectFiles.Count() > 1)
            {
                throw new FileLoadException("More than one project");
            }

            return projectFiles.First();
        }

        /// <summary>
        /// Calculate deltas use Roslyn
        /// </summary>
        /// <param name="source">New source</param>
        /// <param name="sourceName"> filename of source</param>
        /// <param name="filePath"> Path to current solution</param>
        /// <param name="isNewFile"> is file exist in current project</param>
        /// <returns></returns>
        /// <exception cref="InvalidDataException"> Incorrect source file</exception>
        public bool GetDeltas(string source, string sourceName, string filePath, bool isNewFile = false)
        {
            delta = new Update();

            DocumentId documentId = default;
            if (isNewFile)
            {
                documentId = DocumentId.CreateNewId(solution.Projects.First().Id);
                var classPath = Path.GetFullPath(Path.Combine(filePath, $"{sourceName}_second_source.cs"));
                solution = solution.AddDocument(DocumentInfo.Create(
                                                id: documentId,
                                                name: $"{sourceName}_second_source.cs",
                                                loader: new FileTextLoader(classPath, Encoding.UTF8),
                                                filePath: classPath));
            }
            else
            {
                foreach (var projectId in solution.ProjectIds)
                {
                    var project = solution.GetProject(projectId);
                    foreach (var id in project.DocumentIds)
                    {
                        Document document = project.GetDocument(id);
                        if (document.Name == sourceName)
                        {
                            documentId = id;
                            break;
                        }
                    }
                }
            }

            solution = solution.WithDocumentText(documentId, SourceText.From(source, Encoding.UTF8));
            var (updates, hotReloadDiagnostics) = watchHotReloadService.EmitSolutionUpdate(solution, CancellationToken.None);

            if (!hotReloadDiagnostics.IsDefaultOrEmpty)
            {
                foreach (var diagnostic in hotReloadDiagnostics)
                {
                    Console.WriteLine(diagnostic.ToString());
                }
                Console.WriteLine($"Changes made in project will not be applied while the application is running,\n please change the source file #{filePath} in sources");
                return false;
            }
            delta = updates;
            return true;
        }

        /// <summary>
        /// End session of GetDeltaApi
        /// </summary>
        public bool EndGenDeltaSession()
        {
            try
            {
                watchHotReloadService.EndSession();
            }
            catch(Exception ex)
            {
                Console.WriteLine(ex.ToString());
                return false;
            }
            return true;
        }

        /// <summary>
        /// Write ildelta, pdbdelta, metadatadelta to files
        /// </summary>
        /// <param name="fileName">Filename</param>
        public bool WriteDeltas(string fileName) 
        {
            WriteDeltaToFile(delta.MetadataDelta.ToArray(), $"{fileName}.metadata");
            WriteDeltaToFile(delta.ILDelta.ToArray(), $"{fileName}.il");
            WriteDeltaToFile(delta.PdbDelta.ToArray(), $"{fileName}.pdb");

            return File.Exists($"{fileName}.il") ? true : false;
        }

        /// <summary>d
        /// Write deltas to files
        /// </summary>
        /// <param name="delta">Byte array of delta</param>
        /// <param name="filename">Name of file</param>
        private static void WriteDeltaToFile(byte[] delta, string filename)
        {
            using (FileStream file = new FileStream(filename, FileMode.Create, System.IO.FileAccess.Write))
            {
                file.Write(delta, 0, delta.Length);
            }
        }
    }
}
