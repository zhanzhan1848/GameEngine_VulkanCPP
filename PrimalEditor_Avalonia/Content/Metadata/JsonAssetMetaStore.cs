using System;
using System.IO;
using System.Text;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content.Metadata;

public sealed class JsonAssetMetaStore : IAssetMetaStore
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = null
    };

    public AssetMetaDocument? Read(string assetPath)
    {
        var metaPath = GetMetaPath(assetPath);
        if (!File.Exists(metaPath)) return null;

        try
        {
            var document = JsonSerializer.Deserialize<AssetMetaDocument>(File.ReadAllText(metaPath), Options);
            return document is { IsValid: true } ? document : null;
        }
        catch (Exception ex)
        {
            Logger.Warning($"JsonAssetMetaStore: failed to read {metaPath}: {ex.Message}");
            return null;
        }
    }

    public bool Write(string assetPath, AssetMetaDocument document)
    {
        if (document is not { IsValid: true }) return false;

        var metaPath = GetMetaPath(assetPath);
        var partialPath = metaPath + ".partial";

        try
        {
            var directory = Path.GetDirectoryName(metaPath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

            var json = JsonSerializer.Serialize(document, Options) + Environment.NewLine;
            using (var stream = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }

            File.Move(partialPath, metaPath, overwrite: true);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Warning($"JsonAssetMetaStore: failed to write {metaPath}: {ex.Message}");
            try
            {
                if (File.Exists(partialPath)) File.Delete(partialPath);
            }
            catch (Exception cleanupEx)
            {
                Logger.Warning($"JsonAssetMetaStore: failed to clean {partialPath}: {cleanupEx.Message}");
            }

            return false;
        }
    }

    private static string GetMetaPath(string assetPath) => assetPath + ".meta";
}
