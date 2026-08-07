using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
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
    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new GeometryImportPanelViewModel(defaults);

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var destPath = Path.Combine(request.DestinationDir, Path.GetFileName(request.SourcePath));
        var cookedPath = Path.ChangeExtension(destPath, ".mesh");
        Directory.CreateDirectory(request.DestinationDir);

        var settings = (GeometryImportSettings)request.Settings;

        try { File.Copy(request.SourcePath, destPath, overwrite: true); }
        catch (Exception ex)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destPath,
                Type = AssetType.Mesh,
                Success = false,
                ErrorMessage = ex.Message,
            };
        }

        AssetImportSettingsStore.Save<GeometryImportSettings>(destPath, settings);

        if (!NativeEngineBridge.IsLoaded)
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = true };

        var cookOk = await Task.Run(() => NativeEngineBridge.CookMesh(request.SourcePath, cookedPath, settings), ct);
        if (!cookOk)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destPath,
                Type = AssetType.Mesh,
                Success = false,
                ErrorMessage = "ContentTools cooking failed",
            };
        }

        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
        if (requestId == 0)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destPath,
                Type = AssetType.Mesh,
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
                    DestinationPath = destPath,
                    Type = AssetType.Mesh,
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
            DestinationPath = destPath,
            Type = AssetType.Mesh,
            Success = false,
            ErrorMessage = $"Mesh import timed out after {MeshPollTimeoutMs / 1000}s",
        };
    }
}
