using System.Collections.Generic;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Content.Materials;

[JsonDerivedType(typeof(MaterialScalarParameter), "scalar")]
[JsonDerivedType(typeof(MaterialColorParameter), "color")]
public abstract class MaterialParameterValue
{
}

public sealed class MaterialScalarParameter : MaterialParameterValue
{
    public float Scalar { get; set; }
}

public sealed class MaterialColorParameter : MaterialParameterValue
{
    public float R { get; set; } = 1f;
    public float G { get; set; } = 1f;
    public float B { get; set; } = 1f;
    public float A { get; set; } = 1f;
}

public sealed class MaterialRenderState
{
    public string Surface { get; set; } = "Opaque";
    public string CullMode { get; set; } = "Back";
    public bool DepthWrite { get; set; } = true;
    public bool DepthTest { get; set; } = true;
}

public sealed class MaterialDocument
{
    public const int CurrentSchemaVersion = 2;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public string Name { get; set; } = "Material";

    [JsonConverter(typeof(AssetGuid.JsonConverter))]
    public AssetGuid ShaderGuid { get; set; }

    public MaterialRenderState RenderState { get; set; } = new();
    public Dictionary<string, MaterialParameterValue> Parameters { get; set; } = new();
    public Dictionary<string, AssetGuid> Textures { get; set; } = new();

    /// <summary>
    /// Optional relative path to the sidecar graph JSON that lives next to
    /// this material file (e.g. <c>foo.material.graph.json</c>). Null/empty
    /// means this material has no node graph — fall back to the v1 path
    /// that maps <see cref="Parameters"/> / <see cref="Textures"/> straight
    /// onto the engine material.
    /// </summary>
    public string? GraphFile { get; set; }

    [JsonIgnore]
    public bool HasGraph => !string.IsNullOrWhiteSpace(GraphFile);

    [JsonIgnore]
    public bool IsValid =>
        SchemaVersion >= 1 && SchemaVersion <= CurrentSchemaVersion &&
        !string.IsNullOrWhiteSpace(Name);

    public float GetScalar(string name, float fallback = 0f) =>
        Parameters.TryGetValue(name, out var value) && value is MaterialScalarParameter scalar ? scalar.Scalar : fallback;

    public IEnumerable<AssetGuid> EnumerateDependencies()
    {
        if (ShaderGuid.Value != System.Guid.Empty) yield return ShaderGuid;
        foreach (var guid in Textures.Values)
            if (guid.Value != System.Guid.Empty) yield return guid;
    }
}
