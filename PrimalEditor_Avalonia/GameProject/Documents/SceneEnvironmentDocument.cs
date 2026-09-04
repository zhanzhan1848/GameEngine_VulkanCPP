using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject.Documents;

/// <summary>
/// Scene-level environment configuration: ambient light, fog, skybox
/// color, tone mapping, exposure, optional main camera reference.
/// Persisted as a sub-document on <see cref="SceneDocument"/>.
/// Adding new fields is non-breaking because the serializer ignores
/// missing members by default, so old v2 scenes load with defaults.
/// </summary>
public sealed class SceneEnvironmentDocument
{
    [JsonPropertyName("AmbientR")] public float AmbientR { get; set; } = 0.05f;
    [JsonPropertyName("AmbientG")] public float AmbientG { get; set; } = 0.05f;
    [JsonPropertyName("AmbientB")] public float AmbientB { get; set; } = 0.05f;
    [JsonPropertyName("AmbientIntensity")] public float AmbientIntensity { get; set; } = 1.0f;

    [JsonPropertyName("FogEnabled")] public bool FogEnabled { get; set; }
    [JsonPropertyName("FogR")] public float FogR { get; set; } = 0.7f;
    [JsonPropertyName("FogG")] public float FogG { get; set; } = 0.75f;
    [JsonPropertyName("FogB")] public float FogB { get; set; } = 0.8f;
    [JsonPropertyName("FogNear")] public float FogNear { get; set; } = 10f;
    [JsonPropertyName("FogFar")] public float FogFar { get; set; } = 200f;
    [JsonPropertyName("FogDensity")] public float FogDensity { get; set; } = 0.01f;

    [JsonPropertyName("SkyboxEnabled")] public bool SkyboxEnabled { get; set; }
    [JsonPropertyName("SkyR")] public float SkyR { get; set; } = 0.2f;
    [JsonPropertyName("SkyG")] public float SkyG { get; set; } = 0.4f;
    [JsonPropertyName("SkyB")] public float SkyB { get; set; } = 0.8f;

    [JsonPropertyName("ToneMapping")] public string ToneMapping { get; set; } = "ACES";
    [JsonPropertyName("Exposure")] public float Exposure { get; set; } = 1.0f;

    [JsonPropertyName("MainCameraEntityGuid")]
    public string? MainCameraEntityGuid { get; set; }

    [JsonPropertyName("EnvMapPath")]
    public string EnvMapPath { get; set; } = string.Empty;

    /// <summary>静态场景：冻结 DDGI probe 更新（plan §2 A10）。烘焙走 GIEditorPanel。</summary>
    [JsonPropertyName("StaticGIFrozen")]
    public bool StaticGIFrozen { get; set; }
}