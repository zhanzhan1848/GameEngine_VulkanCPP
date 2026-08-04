using System;
using System.IO;
using System.Text;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content.Shaders;

public static class ShaderSerializer
{
    public const int CurrentSchemaVersion = ShaderDocument.CurrentSchemaVersion;

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static void Write(string shaderPath, ShaderDocument document)
    {
        if (document is not { IsValid: true })
            throw new InvalidDataException($"Shader document is invalid: {shaderPath}");

        var partialPath = shaderPath + ".partial";
        try
        {
            var directory = Path.GetDirectoryName(shaderPath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var json = JsonSerializer.Serialize(document, Options) + Environment.NewLine;
            using (var stream = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }
            var validation = JsonSerializer.Deserialize<ShaderDocument>(File.ReadAllText(partialPath), Options);
            if (validation is not { IsValid: true })
                throw new InvalidDataException($"Shader document validation failed: {shaderPath}");
            File.Move(partialPath, shaderPath, overwrite: true);
        }
        catch
        {
            try { if (File.Exists(partialPath)) File.Delete(partialPath); } catch { }
            throw;
        }
    }

    public static ShaderDocument? Read(string shaderPath)
    {
        if (!File.Exists(shaderPath)) return null;
        try
        {
            var document = JsonSerializer.Deserialize<ShaderDocument>(File.ReadAllText(shaderPath), Options);
            if (document is { IsValid: true }) return document;
            Logger.Warning($"Shader document is invalid: {shaderPath}");
            return null;
        }
        catch (Exception ex)
        {
            Logger.Warning($"Shader document load failed for {shaderPath}: {ex.Message}");
            return null;
        }
    }
}
