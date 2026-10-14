using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// ai-navigation S1（2026-10-15）：导航代理组件——把实体声明为导航角色。
/// Radius/Height 参与导航烘焙（网格腐蚀/净空，与物理碰撞尺度同口径）；
/// Speed 是运行时移动速度（Lua nav.move_to 可按任务覆盖）。约束：navagent
/// 实体不要再挂 dynamic RigidBody（导航与物理会同时驱动同一 transform）。
/// CLI-first：先经组件命令/JSON 挂载，Inspector 段随后补（同 audio S1）。
/// </summary>
[JsonDerivedType(typeof(NavAgentComponent), typeDiscriminator: "navAgent")]
public sealed class NavAgentComponent : Component
{
    /// <summary>导航半径（米，缩放 1 时）——烘焙腐蚀用，建议与碰撞半径一致。</summary>
    [JsonPropertyName("Radius")]
    public float Radius { get; set; } = 0.4f;

    /// <summary>净空高度（米）——低顶棚下不可走判定。</summary>
    [JsonPropertyName("Height")]
    public float Height { get; set; } = 1.8f;

    /// <summary>默认移动速度（米/秒）；nav.move_to 第 4 参可覆盖。</summary>
    [JsonPropertyName("Speed")]
    public float Speed { get; set; } = 2f;

    // 单选语义（S1 无多选属性合并编辑；同 MSAudioSource 的最小形状）。
    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSNavAgent(msEntity, this);
}

/// <summary>ai-navigation S1：多选包装（最小形状——属性合并编辑与 Inspector 段后续补）。</summary>
public sealed class MSNavAgent : ViewModels.ViewModelBase, IMSComponent
{
    public string? AttachedHint { get; private set; }

    public MSNavAgent(MSEntity msEntity, NavAgentComponent first)
    {
        Refresh();
    }

    public void Refresh()
    {
        AttachedHint = "nav agent attached";
        OnPropertyChanged(nameof(AttachedHint));
    }
}
