using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.GameProject.Documents;

namespace PrimalEditor_Avalonia.Content.Prefabs;

/// <summary>
/// Prefab document. A prefab is just a collection of
/// <see cref="EntityDocument"/> entries — exactly the layout a
/// <see cref="GameProject.Scene"/> stores — so we serialise the
/// same shape and add a wrapping <see cref="PrefabDocument"/>
/// envelope for <c>Guid</c> + schema version + description.
///
/// Prefabs are referenced by entities through
/// <see cref="Components.PrefabInstanceComponent"/>. The asset is
/// saved via <see cref="PrefabSerializer"/>.
/// </summary>
public sealed class PrefabDocument
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;

    public string Name { get; set; } = "Prefab";

    [JsonConverter(typeof(AssetGuid.JsonConverter))]
    public AssetGuid Guid { get; set; }

    public List<EntityDocument> Entities { get; set; } = new();

    [JsonIgnore]
    public bool IsValid =>
        SchemaVersion == CurrentSchemaVersion &&
        Guid.Value != new Guid() &&
        !string.IsNullOrWhiteSpace(Name);
}

public static class PrefabSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new AssetGuid.JsonConverter() },
    };

    public static void Write(string prefabPath, PrefabDocument document)
    {
        if (document is not { IsValid: true })
            throw new InvalidDataException($"Prefab document is invalid: {prefabPath}");

        var partial = prefabPath + ".partial";
        try
        {
            var dir = Path.GetDirectoryName(prefabPath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            var json = JsonSerializer.Serialize(document, Options) + Environment.NewLine;
            using (var stream = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }
            var verify = JsonSerializer.Deserialize<PrefabDocument>(File.ReadAllText(partial), Options);
            if (verify is not { IsValid: true })
                throw new InvalidDataException($"Prefab document validation failed: {prefabPath}");
            File.Move(partial, prefabPath, overwrite: true);
            Logger.Info($"PrefabSerializer: wrote {prefabPath}");
        }
        catch
        {
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            throw;
        }
    }

    public static PrefabDocument? Read(string prefabPath)
    {
        if (!File.Exists(prefabPath)) return null;
        try
        {
            var document = JsonSerializer.Deserialize<PrefabDocument>(File.ReadAllText(prefabPath), Options);
            return document is { IsValid: true } ? document : null;
        }
        catch (Exception ex)
        {
            Logger.Warning($"PrefabSerializer: failed to read {prefabPath}: {ex.Message}");
            return null;
        }
    }
}
