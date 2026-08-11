using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Per-submesh metadata extracted from a cooked .mesh binary by
/// ContentTools and persisted next to the source .fbx so the editor
/// can render the model -> submesh hierarchy without re-cooking.
/// </summary>
public sealed class ModelSubmeshEntry
{
    public string Name { get; set; } = string.Empty;
    public uint LodId { get; set; }
    public uint LodIndex { get; set; }
    public uint MeshIndex { get; set; }
    public uint MaterialIndex { get; set; }
    public uint VertexCount { get; set; }
    public uint IndexCount { get; set; }
}

/// <summary>
/// JSON sidecar written by ContentTools (Geometry.cpp::write_model_sidecar)
/// alongside the cooked .mesh. The editor reads this to display a model's
/// submeshes in the Content Browser.
/// </summary>
public sealed class ModelSidecar
{
    public int Version { get; set; } = 1;
    public string Source { get; set; } = string.Empty;
    public string Mesh { get; set; } = string.Empty;
    public List<ModelSubmeshEntry> Submeshes { get; set; } = new();

    public static ModelSidecar? Load(string sidecarPath)
    {
        if (string.IsNullOrEmpty(sidecarPath) || !File.Exists(sidecarPath)) return null;
        try
        {
            using var fs = File.OpenRead(sidecarPath);
            var doc = JsonSerializer.Deserialize<ModelSidecar>(fs, JsonOptions);
            return doc;
        }
        catch (Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"ModelSidecar: failed to parse {sidecarPath}: {ex.Message}");
            return null;
        }
    }

    public void Save(string sidecarPath)
    {
        var dir = Path.GetDirectoryName(sidecarPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        using var fs = File.Create(sidecarPath);
        JsonSerializer.Serialize(fs, this, JsonOptions);
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };
}
