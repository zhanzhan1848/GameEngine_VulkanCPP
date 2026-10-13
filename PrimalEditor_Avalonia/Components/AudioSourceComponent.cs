using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// audio S1（2026-10-14）：实体音源组件——把 Content 里的音频资产（.wav 等）
/// 挂到实体上。运行语义（RuntimeSceneExporter audio 行 → PrimalPlayer 平台
/// 播放）与实体位置无关（S1 非空间声；空间声/混音分组是后续阶段）。
/// CLI-first：先经组件命令/JSON 挂载，Inspector 段随后补。
/// </summary>
[JsonDerivedType(typeof(AudioSourceComponent), typeDiscriminator: "audioSource")]
public sealed class AudioSourceComponent : Component
{
    /// <summary>音频资产的注册表 GUID。</summary>
    [JsonPropertyName("AudioAssetGuid")]
    public AssetGuid AudioAssetGuid { get; set; }

    /// <summary>线性音量 0..1（播放端钳制）。</summary>
    [JsonPropertyName("Volume")]
    public float Volume { get; set; } = 1f;

    /// <summary>循环播放（播放端 numberOfLoops=-1 语义）。</summary>
    [JsonPropertyName("Loop")]
    public bool Loop { get; set; }

    /// <summary>Player 注入后立即播放；false = 仅打包（脚本/后续触发器播放是
    /// 后续阶段——S1 没有运行时触发 API）。</summary>
    [JsonPropertyName("AutoPlay")]
    public bool AutoPlay { get; set; } = true;

    // 单选语义（S1 无多选属性合并编辑；同 MSScriptableComponent 的最小形状）。
    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSAudioSource(msEntity, this);
}

/// <summary>audio S1：多选包装（最小形状——属性合并编辑与 Inspector 段后续补）。</summary>
public sealed class MSAudioSource : ViewModels.ViewModelBase, IMSComponent
{
    public string? AttachedHint { get; private set; }

    public MSAudioSource(MSEntity msEntity, AudioSourceComponent first)
    {
        Refresh();
    }

    public void Refresh()
    {
        AttachedHint = "audio source attached";
        OnPropertyChanged(nameof(AttachedHint));
    }
}
