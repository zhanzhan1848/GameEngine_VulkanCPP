using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Import.Settings;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class MeshImporterPlugin : IAssetImporterPlugin
{
    private const int MeshPollTimeoutMs = 60_000;
    private const int MeshPollIntervalMs = 50;

    private static readonly HashSet<string> _exts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend"
    };

    static MeshImporterPlugin()
    {
        AssetImporterRegistry.Register(new MeshImporterPlugin());
    }

    public string DisplayName => "Model Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Mesh };
    public Type SettingsType => typeof(GeometryImportSettings);
    public bool CanClaim(string filePath) => _exts.Contains(Path.GetExtension(filePath));
    public UserControl CreatePanel(IAssetImportSettings defaults)
    {
        var vm = new GeometryImportPanelViewModel(defaults);
        var panel = new GeometryImportPanel { DataContext = vm };
        return panel;
    }

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        // Source file (e.g. .fbx / .obj / .gltf) is NOT copied into the
        // project; the content browser shows the model via its cooked
        // .mesh + .model.json sidecar. The source path is recorded in
        // the sidecar's Source field for traceability and re-cooking.
        Directory.CreateDirectory(request.DestinationDir);

        var sourceStem = Path.GetFileNameWithoutExtension(request.SourcePath);
        var cookedPath = Path.Combine(request.DestinationDir, sourceStem + ".mesh");
        var sidecarPath = Path.Combine(request.DestinationDir, sourceStem + ".model.json");

        var settings = (GeometryImportSettings)request.Settings;

        if (!NativeEngineBridge.IsLoaded)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "ContentTools not loaded; cannot cook mesh.",
            };
        }

        var pod = settings.ToPOD();
        var cookOk = await Task.Run(() => NativeEngineBridge.CookMeshWithPipeline(request.SourcePath, cookedPath, pod), ct);
        if (!cookOk)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "ContentTools cooking failed",
            };
        }

        if (pod.ValidationErrors > 0 || pod.ValidationWarnings > 0)
        {
            Logger.Warning(
                $"Mesh import: {Path.GetFileName(request.SourcePath)} cooked with " +
                $"{pod.ValidationErrors} validation errors and {pod.ValidationWarnings} warnings " +
                $"(meshes={pod.MeshCount}, meshlets={pod.MeshletCount}).");
        }

        // The .model.json sidecar is already written by ContentTools'
        // CookMeshWithPipeline (ContentTools.cpp:762) using real per-mesh
        // metadata. We only need to mint the asset GUID + import settings
        // here; the sidecar itself is the cooker's responsibility.

        // Save .meta against the sidecar (not the cooked .mesh): the
        // registry registers the sidecar as the user-visible asset, and
        // .mesh is a hidden cooked artifact. Keeping .meta with the
        // user-facing file prevents GUID drift if the mesh is regenerated.
        AssetImportSettingsStore.Save<GeometryImportSettings>(sidecarPath, settings);

        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
        if (requestId == 0)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "Mesh loader rejected the request (job system down?)",
            };
        }

        var deadline = DateTime.UtcNow.AddMilliseconds(MeshPollTimeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
            {
                return new ImportedAsset
                {
                    SourcePath = request.SourcePath,
                    DestinationPath = sidecarPath,
                    Type = AssetType.Model,
                    Success = status == NativeEngineBridge.MeshImportResultStatus.Success,
                    NativeHandle = meshId,
                    ErrorMessage = status switch
                    {
                        NativeEngineBridge.MeshImportResultStatus.Failed => "Engine create_resource() returned invalid_id",
                        NativeEngineBridge.MeshImportResultStatus.NotFound => "Internal: request id lost (engine reloaded?)",
                        _ => null,
                    },
                };
            }
            await Task.Delay(MeshPollIntervalMs, ct);
        }

        return new ImportedAsset
        {
            SourcePath = request.SourcePath,
            DestinationPath = sidecarPath,
            Type = AssetType.Model,
            Success = false,
            ErrorMessage = $"Mesh import timed out after {MeshPollTimeoutMs / 1000}s",
        };
    }
}
