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
    Texture,
    Mesh,
    Audio,
    Script,
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

    public static AssetType DetectType(string filePath)
    {
        var ext = Path.GetExtension(filePath);
        if (TextureExtensions.Contains(ext)) return AssetType.Texture;
        if (MeshExtensions.Contains(ext)) return AssetType.Mesh;
        if (AudioExtensions.Contains(ext)) return AssetType.Audio;
        if (ScriptExtensions.Contains(ext)) return AssetType.Script;
        return AssetType.Unknown;
    }

    /// <summary>
    /// Copy a file into the project's Content directory and trigger native import
    /// (if the engine is loaded).
    /// </summary>
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
                asset.ErrorMessage = "GPU upload pending (renderer not initialized)";
                Logger.Debug($"Imported {fileName} to disk; GPU upload deferred: {result?.ErrorMessage ?? "engine returned no result"}");
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

            if (!NativeEngineBridge.IsLoaded)
            {
                Logger.Info($"Imported {fileName} to disk (cooked: {Path.GetFileName(cookedPath)})");
                return asset;
            }

            asset.NativeHandle = NativeEngineBridge.ImportMesh(cookedPath);
            if (asset.NativeHandle != 0)
            {
                Logger.Info($"Imported mesh: {fileName} → {Path.GetFileName(cookedPath)} (handle: {asset.NativeHandle})");
            }
            else
            {
                asset.ErrorMessage = "Mesh upload pending (renderer not initialized)";
                Logger.Debug($"Imported {fileName}; mesh upload deferred (renderer not ready)");
            }
        }
        else
        {
            Logger.Info($"Imported (no native loader): {fileName}");
        }

        return asset;
    }

    /// <summary>
    /// Batch import all assets from a directory.
    /// </summary>
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