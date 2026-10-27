using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// 玩法规则骨架 S1（gameplay-rules.md）：L0 数据契约的编辑器侧承载——
/// 状态机（状态表+转移表+guard）与独立触发器，序列化进 .scene（文档模型+
/// Undo），运行时经 RuntimeSceneExporter 写 sm* 行、Player 构建、
/// Engine/Gameplay 解释器每 tick 求值。谓词是封闭集（prop/dist/timer/count），
/// 新类型必须经契约注册（引擎+导出器+Player 三处同步），不为样例扩集。
/// CLI-first：先经组件命令/JSON 挂载，Inspector 段随后补（同 navAgent）。
/// </summary>
// 多态注册在基类 Component.cs（JsonDerivedType 必须挂在开放多态的基类型上；
// sealed 派生类上挂会被 System.Text.Json 判为不支持多态）。
public sealed class GameplayRulesComponent : Component
{
    /// <summary>初始状态名（必须出现在 States；空 = 无机器段，仅触发器）。</summary>
    [JsonPropertyName("InitialState")]
    public string InitialState { get; set; } = "";

    /// <summary>状态表（名字即身份；不含空格）。</summary>
    [JsonPropertyName("States")]
    public System.Collections.Generic.List<string> States { get; set; } = new();

    /// <summary>转移表（from→to；guard 全过时自动路径可走，Lua 受控转移只做结构校验）。</summary>
    [JsonPropertyName("Transitions")]
    public System.Collections.Generic.List<GameplayTransition> Transitions { get; set; } = new();

    /// <summary>独立触发器（谓词上升沿发事件；不依赖机器存在）。</summary>
    [JsonPropertyName("Triggers")]
    public System.Collections.Generic.List<GameplayTrigger> Triggers { get; set; } = new();

    /// <summary>任务（S2）：目标+完成/失败条件，复用同一封闭谓词集；
    /// 终态先到先得（条件全真 or Lua game.task.complete/fail）。</summary>
    [JsonPropertyName("Tasks")]
    public System.Collections.Generic.List<GameplayTask> Tasks { get; set; } = new();

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSGameplayRules(msEntity, this);
}

/// <summary>状态转移：From→To；Event 非空时转移完成发事件；Guards 全 AND。</summary>
public sealed class GameplayTransition
{
    [JsonPropertyName("From")]
    public string From { get; set; } = "";

    [JsonPropertyName("To")]
    public string To { get; set; } = "";

    /// <summary>转移完成时发射的事件名（空 = 不发）。</summary>
    [JsonPropertyName("Event")]
    public string Event { get; set; } = "";

    /// <summary>manual = Lua 决策边：不参与自动求值，仅供 game.state.transition。</summary>
    [JsonPropertyName("Manual")]
    public bool Manual { get; set; }

    [JsonPropertyName("Guards")]
    public System.Collections.Generic.List<GameplayPredicate> Guards { get; set; } = new();
}

/// <summary>触发器：谓词上升沿（false→true）发事件。once=首次后永久解除；
/// refire=谓词回落后重新武装。</summary>
public sealed class GameplayTrigger
{
    [JsonPropertyName("Event")]
    public string Event { get; set; } = "";

    /// <summary>"once" 或 "refire"。</summary>
    [JsonPropertyName("Mode")]
    public string Mode { get; set; } = "once";

    [JsonPropertyName("Predicate")]
    public GameplayPredicate Predicate { get; set; } = new();
}

/// <summary>任务契约：Id 业务身份；Complete/Fail 各为 AND 条件集。
/// timer/count 谓词按任务创建以来计（与机器的状态作用域区分）。</summary>
public sealed class GameplayTask
{
    [JsonPropertyName("Id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("Complete")]
    public System.Collections.Generic.List<GameplayPredicate> Complete { get; set; } = new();

    [JsonPropertyName("Fail")]
    public System.Collections.Generic.List<GameplayPredicate> Fail { get; set; } = new();
}

/// <summary>
/// 封闭谓词集（四类，与 Engine/Gameplay/GameplayTypes.h 一一对应）：
///   prop   —— EntityA（""/"self"=自身）的脚本属性 Key 与 Value 比较
///   dist   —— EntityA 与 EntityB 的 transform 距离与 Value 比较
///   timer  —— 机器=当前状态停留秒数；独立触发器=创建以来秒数
///   count  —— 事件 Key 的计数（机器按当前状态进入以来）与 Value 比较
/// Op ∈ lt/lte/gt/gte/eq/neq。名字/键不含空格（行协议约束）。
/// </summary>
public sealed class GameplayPredicate
{
    [JsonPropertyName("Kind")]
    public string Kind { get; set; } = "prop";

    [JsonPropertyName("Op")]
    public string Op { get; set; } = "lt";

    [JsonPropertyName("EntityA")]
    public string EntityA { get; set; } = "self";

    [JsonPropertyName("EntityB")]
    public string EntityB { get; set; } = "";

    [JsonPropertyName("Key")]
    public string Key { get; set; } = "";

    [JsonPropertyName("Value")]
    public float Value { get; set; }
}

/// <summary>玩法规则 S1：多选包装（最小形状——属性合并编辑与 Inspector 段后续补）。</summary>
public sealed class MSGameplayRules : ViewModelBase, IMSComponent
{
    public string? AttachedHint { get; private set; }

    public MSGameplayRules(MSEntity msEntity, GameplayRulesComponent first)
    {
        Refresh();
    }

    public void Refresh()
    {
        AttachedHint = "gameplay rules attached";
        OnPropertyChanged(nameof(AttachedHint));
    }
}
