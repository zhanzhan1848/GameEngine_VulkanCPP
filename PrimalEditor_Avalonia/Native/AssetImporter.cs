using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native;

public enum AssetType
{
    Unknown,
    Folder,
    Texture,
    Mesh,
    Model,
    Audio,
    Script,
    Material,
    Shader,
    Prefab,
    ParticlePreset,
    Pcg,
    Sequence,
}

public class ImportedAsset
{
    public string SourcePath { get; set; } = string.Empty;
    public string DestinationPath { get; set; } = string.Empty;
    public AssetType Type { get; set; }
    public ulong NativeHandle { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public bool Success { get; set; }
    public string? ErrorMessage { get; set; }
}

public static class AssetImporter
{
    private static readonly HashSet<string> TextureExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".ktx", ".ktx2"
    };

    private static readonly HashSet<string> MeshExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend"
    };

    private static readonly HashSet<string> AudioExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".wav", ".mp3", ".ogg", ".flac"
    };

    private static readonly HashSet<string> ScriptExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".py", ".lua", ".cs"
    };

    private const int MeshPollTimeoutMs = 60_000;
    private const int MeshPollIntervalMs = 50;

    public static AssetType DetectType(string filePath)
    {
        var ext = Path.GetExtension(filePath);
        if (TextureExtensions.Contains(ext)) return AssetType.Texture;
        if (MeshExtensions.Contains(ext)) return AssetType.Mesh;
        if (AudioExtensions.Contains(ext)) return AssetType.Audio;
        if (ScriptExtensions.Contains(ext)) return AssetType.Script;
        return AssetType.Unknown;
    }

    public static async Task<ImportedAsset?> ImportAsync(
        string sourcePath, string projectContentPath, string? subfolder = null)
    {
        if (!File.Exists(sourcePath))
        {
            Logger.Error($"Asset not found: {sourcePath}");
            return null;
        }

        var type = DetectType(sourcePath);
        var fileName = Path.GetFileName(sourcePath);

        var destDir = string.IsNullOrEmpty(subfolder)
            ? projectContentPath
            : Path.Combine(projectContentPath, subfolder);

        Directory.CreateDirectory(destDir);
        var destPath = Path.Combine(destDir, fileName);

        try
        {
            File.Copy(sourcePath, destPath, overwrite: true);
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to copy asset: {fileName}", ex);
            return new ImportedAsset
            {
                SourcePath = sourcePath,
                DestinationPath = destPath,
                Type = type,
                Success = false,
                ErrorMessage = ex.Message,
            };
        }

        var asset = new ImportedAsset
        {
            SourcePath = sourcePath,
            DestinationPath = destPath,
            Type = type,
            Success = true,
        };

        if (!NativeEngineBridge.IsLoaded)
        {
            Logger.Info($"Imported (no engine): {fileName} → {subfolder ?? "Content"}");
            return asset;
        }

        if (type == AssetType.Texture)
        {
            var result = await Task.Run(() => NativeEngineBridge.ImportTexture(destPath));
            if (result.HasValue && result.Value.Success != 0)
            {
                asset.NativeHandle = result.Value.Handle;
                asset.Width = (int)result.Value.Width;
                asset.Height = (int)result.Value.Height;
                Logger.Info($"Imported texture: {fileName} ({asset.Width}x{asset.Height})");
            }
            else
            {
                var err = result?.ErrorMessage;
                asset.ErrorMessage = string.IsNullOrEmpty(err)
                    ? "GPU upload failed"
                    : $"GPU upload failed: {err}";
                asset.Success = false;
                Logger.Warning($"Texture import failed: {fileName} — {asset.ErrorMessage}");
            }
        }
        else if (type == AssetType.Mesh)
        {
            var cookedPath = Path.ChangeExtension(destPath, ".mesh");
            var ok = await Task.Run(() => NativeEngineBridge.CookMesh(destPath, cookedPath));
            if (!ok)
            {
                asset.ErrorMessage = "ContentTools cooking failed";
                asset.Success = false;
                Logger.Warning($"Mesh cooking failed: {fileName}");
                return asset;
            }

            var requestId = NativeEngineBridge.ImportMesh(cookedPath);
            if (requestId == 0)
            {
                asset.ErrorMessage = "Mesh loader rejected the request (job system down?)";
                asset.Success = false;
                Logger.Warning($"Mesh import failed: {fileName} — loader rejected the request");
                return asset;
            }

            Logger.Debug($"Mesh import queued: {fileName} → {Path.GetFileName(cookedPath)} (request {requestId})");

            var (meshContentId, finalStatus) = await WaitForMeshAsync(requestId);
            if (finalStatus == NativeEngineBridge.MeshImportResultStatus.Success)
            {
                asset.NativeHandle = meshContentId;
                Logger.Info($"Imported mesh: {fileName} → {Path.GetFileName(cookedPath)} (mesh_content_id: 0x{meshContentId:X})");
            }
            else
            {
                asset.ErrorMessage = finalStatus switch
                {
                    NativeEngineBridge.MeshImportResultStatus.Failed => "Engine create_resource() returned invalid_id",
                    NativeEngineBridge.MeshImportResultStatus.NotFound => "Internal: request id lost (engine reloaded?)",
                    _ => $"Mesh import timed out after {MeshPollTimeoutMs / 1000}s"
                };
                asset.Success = false;
                Logger.Warning($"Mesh import failed: {fileName} — {asset.ErrorMessage}");
            }
        }
        else
        {
            Logger.Info($"Imported (no native loader): {fileName}");
        }

        return asset;
    }

    private static async Task<(ulong meshContentId, NativeEngineBridge.MeshImportResultStatus status)> WaitForMeshAsync(ulong requestId)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(MeshPollTimeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
                return (meshId, status);
            await Task.Delay(MeshPollIntervalMs);
        }
        return (0, NativeEngineBridge.MeshImportResultStatus.Pending);
    }

    public static async Task<List<ImportedAsset>> ImportDirectoryAsync(
        string sourceDir, string projectContentPath, string? subfolder = null)
    {
        var results = new List<ImportedAsset>();
        if (!Directory.Exists(sourceDir)) return results;

        var files = Directory.GetFiles(sourceDir);
        foreach (var file in files)
        {
            if (DetectType(file) == AssetType.Unknown) continue;
            var asset = await ImportAsync(file, projectContentPath, subfolder);
            if (asset != null) results.Add(asset);
        }
        return results;
    }
}