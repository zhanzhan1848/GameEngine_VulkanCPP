using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content.Metadata;

/// <summary>
/// Detects whether a sidecar <c>.meta</c> file uses the legacy binary
/// "PRIM" encoding and rewrites it to the JSON form on demand. The
/// AssetRegistry calls this on every Register so old projects open
/// without any manual "import migration" step.
///
/// Migration is intentionally permissive — if the sidecar exists but
/// matches neither format, the method returns false and the caller
/// treats it as "no meta" (a fresh Guid will be minted on the next save).
/// </summary>
public static class LegacyMetaMigrator
{
    /// <summary>
    /// Return true if <paramref name="metaPath"/> carries the legacy
    /// "PRIM" magic bytes in its first 4 bytes. Used by callers that
    /// want to short-circuit before allocating a full read.
    /// </summary>
    public static bool IsLegacyFormat(string metaPath)
    {
        if (!File.Exists(metaPath)) return false;
        try
        {
            using var fs = File.OpenRead(metaPath);
            Span<byte> head = stackalloc byte[4];
            return fs.Read(head) == 4 && head[0] == 'P' && head[1] == 'R' && head[2] == 'I' && head[3] == 'M';
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Convert a legacy binary <c>.meta</c> into the JSON form in-place.
    /// Returns the resulting <see cref="AssetMetaDocument"/> so callers
    /// can pick up its GUID + settings without re-reading; returns null
    /// when migration fails so the caller can fall back to minting a
    /// fresh GUID.
    /// </summary>
    public static AssetMetaDocument? Migrate(string assetPath)
    {
        var metaPath = assetPath + ".meta";
        if (!IsLegacyFormat(metaPath))
        {
            // Not legacy: the JSON reader already understands it.
            return null;
        }

        var importer = InferImporterFromExtension(assetPath);
        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(metaPath, importer);
        if (!result.Success || result.Document == null)
        {
            Logger.Warning($"LegacyMetaMigrator: {metaPath} is legacy but unreadable; cannot migrate");
            return null;
        }

        // Rewrite the sidecar using JsonAssetMetaStore. The JSON
        // form is the single source of truth now, so subsequent
        // AssetRegistry reads won't re-pay the binary parsing cost.
        var store = new JsonAssetMetaStore();
        var ok = store.Write(assetPath, result.Document);
        if (!ok)
        {
            Logger.Warning($"LegacyMetaMigrator: failed to write JSON sidecar at {assetPath}");
            return null;
        }
        Logger.Info($"LegacyMetaMigrator: migrated {Path.GetFileName(metaPath)} (importer={importer})");
        return result.Document;
    }

    private static string InferImporterFromExtension(string assetPath)
    {
        var fileName = Path.GetFileName(assetPath).ToLowerInvariant();
        var ext = Path.GetExtension(assetPath).ToLowerInvariant();
        return ext switch
        {
            ".png" or ".jpg" or ".jpeg" or ".bmp" or ".tga" or ".dds" or ".ktx" or ".ktx2" or ".hdr" or ".exr" => "TextureImporter",
            ".mesh" or ".fbx" or ".obj" or ".gltf" or ".glb" or ".dae" or ".blend" => "GeometryImporter",
            ".material" => "MaterialImporter",
            ".shader" => "ShaderImporter",
            _ => fileName.EndsWith(".model.json", StringComparison.Ordinal) ? "GeometryImporter" : "UnknownImporter",
        };
    }
}
