using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// 编辑器侧粒子发射器组件（plan §6.3 B4 P1）。文档模型字段是单一事实源；
/// 同步时引擎用 PresetName 的标量场填默认 config，再覆盖 SpawnRate。
/// 曲线编辑（P3）与全参数面板（P2 后半）后续接入。
/// </summary>
public sealed class ParticleEmitterComponent : Component
{
    /// <summary>预设名（内置：fire/smoke/explosion/magic_sparkle/rain/snow/dust/bubbles，
    /// 或 .particle.json 装载的具名预设）。空串回退 "fire"。</summary>
    [JsonPropertyName("PresetName")]
    public string PresetName { get; set; } = "fire";

    private float _spawnRate = 50f;
    [JsonPropertyName("SpawnRate")]
    public float SpawnRate
    {
        get => _spawnRate;
        set => _spawnRate = value >= 0 ? value : 0f;
    }

    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled { get; set; } = true;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        // P2：多选编辑（SpawnRate 混合值）接入 MS 包装后实现。
        throw new System.NotSupportedException("Particle multi-selection is not yet implemented.");
}
