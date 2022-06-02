using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Host;
using Microsoft.CodeAnalysis.MSBuild;

namespace NetcoreDbgTest.GetDeltaApi
{
    /// <summary>
    /// The capabilities that the runtime has with respect to edit and continue
    /// </summary>
    public sealed class EditAndContinueCapabilities 
    {
        /// <summary>
        /// Edit and continue is generally available with the set of capabilities that Mono 6, .NET Framework and .NET 5 have in common.
        /// </summary>
        public static string Baseline = "Baseline";

        /// <summary>
        /// Adding a static or instance method to an existing type.
        /// </summary>
        public static string AddMethodToExistingType = "AddMethodToExistingType";

        /// <summary>
        /// Adding a static field to an existing type.
        /// </summary>
        public static string AddStaticFieldToExistingType = "AddStaticFieldToExistingType";

        /// <summary>
        /// Adding an instance field to an existing type.
        /// </summary>
        public static string AddInstanceFieldToExistingType = "AddInstanceFieldToExistingType";

        /// <summary>
        /// Creating a new type definition.
        /// </summary>
        public static string NewTypeDefinition = "NewTypeDefinition";

        /// <summary>
        /// Adding, updating and deleting of custom attributes (as distinct from pseudo-custom attributes)
        /// </summary>
        public static string ChangeCustomAttributes = "ChangeCustomAttributes";

        /// <summary>
        /// Whether the runtime supports updating the Param table, and hence related edits (eg parameter renames)
        /// </summary>
        public static string UpdateParameters = "UpdateParameters";
    }
    public readonly struct Update
    {
        public readonly Guid ModuleId;
        public readonly ImmutableArray<byte> ILDelta;
        public readonly ImmutableArray<byte> MetadataDelta;
        public readonly ImmutableArray<byte> PdbDelta;
        public readonly dynamic SequencePoints;

        public Update(Guid moduleId, ImmutableArray<byte> ilDelta, ImmutableArray<byte> metadataDelta, ImmutableArray<byte> pdbDelta, dynamic sequencePoints)
        {
            ModuleId = moduleId;
            ILDelta = ilDelta;
            MetadataDelta = metadataDelta;
            PdbDelta = pdbDelta;
            SequencePoints = sequencePoints;
        }
    }

    public class WatchHotReloadService
    {
        public object current_project = null;
        public object editAndContinueWorkspaceServiceInstance = null;
        private static ImmutableArray<string> s_net5RuntimeCapabilities = ImmutableArray.Create(EditAndContinueCapabilities.Baseline, EditAndContinueCapabilities.AddInstanceFieldToExistingType,
                                                                                        EditAndContinueCapabilities.AddStaticFieldToExistingType, EditAndContinueCapabilities.AddMethodToExistingType,
                                                                                        EditAndContinueCapabilities.NewTypeDefinition);

        private static ImmutableArray<string> s_net6RuntimeCapabilities = s_net5RuntimeCapabilities.AddRange(new[] { EditAndContinueCapabilities.ChangeCustomAttributes, EditAndContinueCapabilities.UpdateParameters });

        /// <summary>
        /// Initialize WatchHotReloadService
        /// </summary>
        /// <param name="project">Current project</param>
        public void InitializeService(Project project)
        {
            try
            {
                current_project = project;
                var encWorkspaceServiceType = Type.GetType("Microsoft.CodeAnalysis.EditAndContinue.EditAndContinueWorkspaceService, Microsoft.CodeAnalysis.Features");
                editAndContinueWorkspaceServiceInstance = encWorkspaceServiceType.GetConstructor(Array.Empty<Type>()).Invoke(parameters: Array.Empty<object>());
            }
            catch (Exception ex)
            {
                throw new Exception(ex.Message);
            }
        }

        /// <summary>
        /// Start debugging session
        /// </summary>
        /// <param name="solution">Current solution</param>
        /// <param name="cancellationToken">cancellation token</param>
        public void StartSessionAsync(Solution solution, CancellationToken cancellationToken)
        {

            var defaultDocumentTrackingServiceType = Type.GetType("Microsoft.CodeAnalysis.SolutionCrawler.DefaultDocumentTrackingService, Microsoft.CodeAnalysis.Features");
            var defaultDocumentTrackingServiceConstructor = defaultDocumentTrackingServiceType.GetConstructor(Array.Empty<Type>()).Invoke(parameters: Array.Empty<object>());
            var emptyImmutableArrayofDocumentId = defaultDocumentTrackingServiceConstructor.GetType().GetMethod("GetVisibleDocuments").Invoke(obj: defaultDocumentTrackingServiceConstructor, parameters: Array.Empty<object>());
            var debuggerService = Type.GetType("Microsoft.CodeAnalysis.ExternalAccess.Watch.Api.WatchHotReloadService, Microsoft.CodeAnalysis.Features")
                                      .GetNestedType(name: "DebuggerService", bindingAttr: BindingFlags.NonPublic)
                                      .GetConstructor(types: new Type[] { s_net6RuntimeCapabilities.GetType() })
                                      .Invoke(parameters: new object[] { s_net6RuntimeCapabilities });
            
            var startDebuggingSessionAsync = editAndContinueWorkspaceServiceInstance.GetType()
                                                        .GetMethod("StartDebuggingSessionAsync")
                                                        .Invoke(obj: editAndContinueWorkspaceServiceInstance,
                                                                parameters: new object[] { solution, debuggerService, ImmutableArray<DocumentId>.Empty, false, true, CancellationToken.None });
        }

        /// <summary>
        /// Emits updates for all projects that differ between the given <paramref name="solution"/> snapshot and the one given to the previous successful call or
        /// the one passed to <see cref="StartSessionAsync(Solution, CancellationToken)"/> for the first invocation.
        /// </summary>
        /// <param name="solution">Solution snapshot.</param>
        /// <param name="cancellationToken">Cancellation token.</param>
        /// <returns>
        /// Updates and Rude Edit diagnostics. Does not include syntax or semantic diagnostics.
        /// </returns>
        public (Update deltas, object diagnostics) EmitSolutionUpdate(Solution solution, CancellationToken cancellationToken)
        {
            var deltas = new Update();
            object diagnostics = default;
            var activeStatementSpanProvider = Type.GetType("Microsoft.CodeAnalysis.ExternalAccess.Watch.Api.WatchHotReloadService, Microsoft.CodeAnalysis.Features").GetField(name: "s_solutionActiveStatementSpanProvider",
                                                                                                                                                                              bindingAttr: BindingFlags.NonPublic | BindingFlags.Static).GetValue(default);
            var id = editAndContinueWorkspaceServiceInstance.GetType().GetField(name: "s_debuggingSessionId", bindingAttr: BindingFlags.NonPublic | BindingFlags.Static).GetValue(editAndContinueWorkspaceServiceInstance);
            var debuggingSessionId = Type.GetType("Microsoft.CodeAnalysis.EditAndContinue.DebuggingSessionId, Microsoft.CodeAnalysis.Features").GetConstructor(new Type[] { typeof(int) }).Invoke(new object[] { id });
            var emitSolutionUpdateAsync = editAndContinueWorkspaceServiceInstance.GetType().GetMethod("EmitSolutionUpdateAsync").Invoke(obj: editAndContinueWorkspaceServiceInstance,
                                                                                                                                        parameters: new object[] { debuggingSessionId, solution, activeStatementSpanProvider, CancellationToken.None });
            var get_results = emitSolutionUpdateAsync.GetType().GetMethod("get_Result").Invoke(obj: emitSolutionUpdateAsync,
                                                                                               parameters: Array.Empty<object>());
            var newModuleUpdates = get_results.GetType().GetField("ModuleUpdates").GetValue(get_results);
            var getAllDiagnostics = get_results.GetType().GetMethod("GetAllDiagnosticsAsync").Invoke(get_results, new object[] { solution, cancellationToken });
            var updates = newModuleUpdates.GetType().GetProperty("Updates").GetValue(newModuleUpdates);
            var defaultDocumentTrackingServiceType = Type.GetType("Microsoft.CodeAnalysis.SolutionCrawler.DefaultDocumentTrackingService, Microsoft.CodeAnalysis.Features");
            var defaultDocumentTrackingServiceConstructor = defaultDocumentTrackingServiceType.GetConstructor(Array.Empty<Type>()).Invoke(parameters: Array.Empty<object>());
            var emptyImmutableArrayofDocumentId = defaultDocumentTrackingServiceConstructor.GetType().GetMethod("GetVisibleDocuments").Invoke(obj: defaultDocumentTrackingServiceConstructor,
                                                                                                                                              parameters: Array.Empty<object>());
            var managedModuleUpdateStatusReady = Type.GetType("Microsoft.CodeAnalysis.EditAndContinue.Contracts.ManagedModuleUpdateStatus, Microsoft.CodeAnalysis.Features").GetField("Ready").GetValue(newModuleUpdates);

            if (newModuleUpdates.GetType().GetProperty("Status").GetValue(newModuleUpdates).Equals(managedModuleUpdateStatusReady))
            {
                editAndContinueWorkspaceServiceInstance.GetType().GetMethod("CommitSolutionUpdate").Invoke(obj: editAndContinueWorkspaceServiceInstance,
                                                                                               parameters: new object[] { debuggingSessionId, emptyImmutableArrayofDocumentId });
            }
            else
            {
                diagnostics = getAllDiagnostics.GetType().GetProperty("Result").GetValue(getAllDiagnostics);
            }

            var moduleUpdates = (IList)updates;

            for (int i = 0; i < moduleUpdates.Count; i++)
            {
                object moduleUpdate = moduleUpdates[i];
                var updateType = moduleUpdate.GetType();

                deltas = new Update((Guid)updateType.GetProperty("Module").GetValue(moduleUpdate),
                        (ImmutableArray<byte>)updateType.GetProperty("ILDelta").GetValue(moduleUpdate),
                        (ImmutableArray<byte>)updateType.GetProperty("MetadataDelta").GetValue(moduleUpdate),
                        (ImmutableArray<byte>)updateType.GetProperty("PdbDelta").GetValue(moduleUpdate),
                        (dynamic)(updateType.GetProperty("SequencePoints").GetValue(moduleUpdate)));
            }
            return (deltas, diagnostics);
        }

        /// <summary>
        /// End Debugging session
        /// </summary>
        public void EndSession()
        {
            object docs = null;
            var id = editAndContinueWorkspaceServiceInstance.GetType().GetField(name: "s_debuggingSessionId", bindingAttr: BindingFlags.NonPublic | BindingFlags.Static).GetValue(editAndContinueWorkspaceServiceInstance);
            var debuggingSessionId = Type.GetType("Microsoft.CodeAnalysis.EditAndContinue.DebuggingSessionId, Microsoft.CodeAnalysis.Features").GetConstructor(new Type[] { typeof(int) }).Invoke(new object[] { id });
            var endDebuggingSession = editAndContinueWorkspaceServiceInstance.GetType().GetMethod("EndDebuggingSession").Invoke(obj: editAndContinueWorkspaceServiceInstance,
                                                                                                                            parameters: new object[] { debuggingSessionId, docs });           
        }
    }
}