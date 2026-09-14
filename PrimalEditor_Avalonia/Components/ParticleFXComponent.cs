using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// PFX 可编程粒子框架组件（Docs/2026-09-12-particle-framework-architecture.md §4/N1）。
/// 文档模型字段是单一事实源；引擎侧按 AssetPath 加载编译 .pfx_emitter JSON 资产。
/// 暴露参数（ExposedParams）经 PfxSetExposedParam 在运行期覆盖资产默认值。
/// 与旧 ParticleEmitterComponent 并存互斥使用（旧系统冻结维护）。
/// </summary>
public sealed class ParticleFXComponent : Component
{
    /// <summary>.pfx_emitter JSON 资产路径（相对项目或绝对）。空 = 未配置，不同步。</summary>
    [JsonPropertyName("AssetPath")]
    public string AssetPath { get; set; } = string.Empty;

    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled { get; set; } = true;

    /// <summary>暴露参数覆盖（名字 → 值；名字经 Murmur3-32 与引擎 schema 对齐）。</summary>
    [JsonPropertyName("ExposedParams")]
    public Dictionary<string, float> ExposedParams { get; set; } = new();

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        // v1：多选编辑未实现（与 ParticleEmitterComponent 同状态）。
        throw new System.NotSupportedException("ParticleFX multi-selection is not yet implemented.");
}
