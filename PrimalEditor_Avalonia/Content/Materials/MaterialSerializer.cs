using System;
using System.IO;
using System.Text;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;

namespace PrimalEditor_Avalonia.Content.Materials;

public static class MaterialSerializer
{
    public const int CurrentSchemaVersion = MaterialDocument.CurrentSchemaVersion;

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new AssetGuid.JsonConverter() }
    };

    public static void Write(string materialPath, MaterialDocument document)
    {
        if (document is not { IsValid: true })
            throw new InvalidDataException($"Material document is invalid: {materialPath}");

        WriteInternal(materialPath, document);
    }

    /// <summary>
    /// Create a brand-new material file with sensible defaults and no
    /// shader binding. Bypasses <see cref="MaterialDocument.IsValid"/>'s
    /// ShaderGuid check because the user hasn't picked one yet — the
    /// editor opens immediately and the user fills it in.
    /// </summary>
    public static void WriteNew(string materialPath, string name)
    {
        var doc = new MaterialDocument
        {
            Name = string.IsNullOrWhiteSpace(name)
                ? Path.GetFileNameWithoutExtension(materialPath)
                : name.Trim(),
            // ShaderGuid deliberately left at Guid.Empty — editor must
            // bind a shader before the renderer can apply the material.
        };
        WriteInternal(materialPath, doc, validateShaderBinding: false);
    }

    private static void WriteInternal(string materialPath, MaterialDocument document, bool validateShaderBinding = true)
    {
        var partialPath = materialPath + ".partial";
        try
        {
            var directory = Path.GetDirectoryName(materialPath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var json = JsonSerializer.Serialize(document, Options) + Environment.NewLine;
            using (var stream = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }
            // Re-read the just-written bytes so we catch any
            // serializer round-trip anomaly (e.g. an enum string that
            // the next deserialize would refuse). When the caller has
            // explicitly opted out of the shader-binding check
            // (WriteNew), we still want the round-trip to succeed —
            // a malformed schema is still a hard error.
            var validation = JsonSerializer.Deserialize<MaterialDocument>(File.ReadAllText(partialPath), Options);
            if (validation is null)
                throw new InvalidDataException($"Material document validation failed: {materialPath}");
            if (validateShaderBinding && !validation.IsValid)
                throw new InvalidDataException($"Material document validation failed: {materialPath}");
            File.Move(partialPath, materialPath, overwrite: true);
        }
        catch
        {
            try { if (File.Exists(partialPath)) File.Delete(partialPath); } catch { }
            throw;
        }
    }

    public static MaterialDocument? Read(string materialPath)
    {
        if (!File.Exists(materialPath)) return null;
        try
        {
            var document = JsonSerializer.Deserialize<MaterialDocument>(File.ReadAllText(materialPath), Options);
            if (document is { IsValid: true }) return document;
            Logger.Warning($"Material document is invalid: {materialPath}");
            return null;
        }
        catch (Exception ex)
        {
            Logger.Warning($"Material document load failed for {materialPath}: {ex.Message}");
            return null;
        }
    }
}
