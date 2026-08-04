using System;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Components;

[JsonDerivedType(typeof(MaterialOverrideComponent), typeDiscriminator: "materialOverride")]
public class MaterialOverrideComponent : Component
{
    public ObservableCollection<MaterialOverrideSlot> Slots { get; set; } = new();

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSMaterialOverride(msEntity);
}

/// <summary>
/// Per-submesh material override + parameter overrides. The
/// <see cref="SubMeshIndex"/> ties this slot to a specific submesh
/// of the host <c>MeshRenderer</c>; multiple slots at the same
/// index replace one another (last write wins) — the editor UI
/// de-duplicates before saving.
/// </summary>
public sealed class MaterialOverrideSlot
{
    public string Slot { get; set; } = string.Empty;

    /// <summary>Submesh index in the host mesh; -1 means "applies to
    /// the whole material".</summary>
    public int SubMeshIndex { get; set; } = -1;

    [JsonConverter(typeof(AssetGuid.JsonConverter))]
    public AssetGuid MaterialGuid { get; set; } = new AssetGuid(Guid.Empty);

    /// <summary>Scalars/colors overridden on this material. Keyed by
    /// the shader's parameter name. Last write wins on duplicate
    /// keys.</summary>
    public Dictionary<string, MaterialParameterOverride> Parameters { get; set; } = new();
}

/// <summary>
/// One parameter override on a material slot. Wrapped so the
/// inspector can bind to concrete shapes without needing custom
/// JSON converters for each variant.
/// </summary>
public sealed class MaterialParameterOverride
{
    public MaterialParameterType Type { get; set; } = MaterialParameterType.Scalar;

    /// <summary>Scalar value when <see cref="Type"/> is
    /// <see cref="MaterialParameterType.Scalar"/>.</summary>
    public float Scalar { get; set; }

    /// <summary>RGBA values when <see cref="Type"/> is
    /// <see cref="MaterialParameterType.Color"/> (each in 0..1).</summary>
    public float R { get; set; }
    public float G { get; set; }
    public float B { get; set; }
    public float A { get; set; } = 1f;
}

public enum MaterialParameterType
{
    Scalar,
    Color,
}

