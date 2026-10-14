using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.DeviceData;

namespace PrimalEditor_Avalonia.Twin;

/// <summary>孪生节点（twin-tools S1，2026-10-15）：设备/部件语义层。
/// 稳定业务 ID；可链接场景实体（EntityGuid）与设备数据源（DeviceId）。</summary>
public sealed class TwinNode
{
    public string Id { get; init; } = "";
    /// <summary>site / device / part。</summary>
    public string Kind { get; init; } = "device";
    public string Name { get; init; } = "";
    public string? ParentId { get; init; }
    /// <summary>关联场景实体（可空——纯语义节点）。</summary>
    public Guid? EntityGuid { get; init; }
    /// <summary>关联设备数据源（DeviceDataStore 的 DeviceId）。</summary>
    public string? DeviceId { get; init; }
}

/// <summary>
/// 孪生注册表：层级查询（按名/路径定位、子树）。纯数据——不接触引擎。
/// 重复 ID 注册拒绝（InvalidOperationException）；孤儿 parent 容忍并报告。
/// </summary>
public sealed class TwinRegistry
{
    private readonly Dictionary<string, TwinNode> _nodes = new(StringComparer.Ordinal);

    public IReadOnlyCollection<TwinNode> Nodes => _nodes.Values;

    public void Register(TwinNode node)
    {
        if (string.IsNullOrWhiteSpace(node.Id))
            throw new InvalidOperationException("节点 ID 不能为空");
        if (!_nodes.TryAdd(node.Id, node))
            throw new InvalidOperationException($"重复的孪生节点 ID: {node.Id}");
    }

    /// <summary>按名定位（首次命中；同名返回第一个注册的——确定性）。</summary>
    public TwinNode? FindByName(string name) =>
        _nodes.Values.FirstOrDefault(n => n.Name == name);

    /// <summary>路径定位："siteA/deviceB/partC"（按层级名拼接）。</summary>
    public TwinNode? FindByPath(string path)
    {
        var segments = path.Split('/', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0) return null;
        TwinNode? current = null;
        foreach (var segment in segments)
        {
            var next = _nodes.Values.FirstOrDefault(n =>
                n.Name == segment && (current == null ? n.ParentId == null : n.ParentId == current.Id));
            if (next == null) return null;
            current = next;
        }
        return current;
    }

    /// <summary>子树（含自身）。</summary>
    public List<TwinNode> Subtree(TwinNode root)
    {
        var result = new List<TwinNode> { root };
        bool grew = true;
        while (grew)
        {
            grew = false;
            foreach (var n in _nodes.Values)
            {
                if (result.Contains(n) || n.ParentId == null) continue;
                if (result.Any(p => p.Id == n.ParentId))
                {
                    result.Add(n);
                    grew = true;
                }
            }
        }
        return result;
    }

    /// <summary>孤儿节点（parent 未注册）——诊断用，不静默。</summary>
    public List<TwinNode> Orphans() =>
        _nodes.Values.Where(n => n.ParentId != null && !_nodes.ContainsKey(n.ParentId)).ToList();
}

/// <summary>告警规则（twin-tools S1）：对设备信号的时间序列做确定性评估。</summary>
public sealed class AlarmRule
{
    public string RuleId { get; init; } = "";
    public string DeviceId { get; init; } = "";
    public string Signal { get; init; } = "";
    /// <summary>阈值比较符：&gt; / &lt; / &gt;= / &lt;= 。</summary>
    public string Comparator { get; init; } = ">";
    public double Threshold { get; init; }
    /// <summary>触发严重度：info / warning / alarm。</summary>
    public string Severity { get; init; } = "warning";
    /// <summary>滞回带（同单位）：越过 Threshold 触发后，须回落到
    /// Threshold−hysteresis（&gt; 型）以下才解除——防临界抖动。</summary>
    public double Hysteresis { get; init; }
    /// <summary>持续时长（秒）：条件连续保持该时长才触发；0 = 立即。</summary>
    public double SustainSeconds { get; init; }

    public bool Condition(double v) => Comparator switch
    {
        ">" => v > Threshold,
        "<" => v < Threshold,
        ">=" => v >= Threshold,
        "<=" => v <= Threshold,
        _ => false,
    };

    public bool ClearCondition(double v) => Comparator switch
    {
        ">" => v <= Threshold - Hysteresis,
        "<" => v >= Threshold + Hysteresis,
        ">=" => v < Threshold - Hysteresis,
        "<=" => v > Threshold + Hysteresis,
        _ => false,
    };
}

/// <summary>告警事件（区间语义：firedAt ≤ clearedAt；未解除 = clearedAt null）。</summary>
public sealed class AlarmEvent
{
    public string RuleId = "";
    public DateTime FiredAt;
    public DateTime? ClearedAt;
}

/// <summary>
/// 告警评估器：对 store 历史窗口逐样本推进（确定性，与墙钟无关）。
/// 状态机：Normal → (条件满足且持续 SustainSeconds) → Active → (回落出滞回带) → Normal。
/// </summary>
public static class AlarmEvaluator
{
    public static List<AlarmEvent> Evaluate(
        DeviceDataStore store, IReadOnlyList<AlarmRule> rules,
        string deviceId, string signal, DateTime fromUtc, DateTime toUtc,
        double nominalPeriodSeconds)
    {
        var (samples, _) = store.Window(deviceId, signal, fromUtc, toUtc, nominalPeriodSeconds);
        var events = new List<AlarmEvent>();
        foreach (var rule in rules.Where(r => r.DeviceId == deviceId && r.Signal == signal))
        {
            var active = false;
            AlarmEvent? current = null;
            DateTime? conditionSince = null;
            foreach (var s in samples)
            {
                if (!active)
                {
                    if (rule.Condition(s.Value))
                    {
                        conditionSince ??= s.TimestampUtc;
                        var held = (s.TimestampUtc - conditionSince.Value).TotalSeconds;
                        if (held >= rule.SustainSeconds)
                        {
                            current = new AlarmEvent
                            {
                                RuleId = rule.RuleId,
                                FiredAt = conditionSince.Value,   // 持续条件起点（可追溯）
                            };
                            events.Add(current);
                            active = true;
                            conditionSince = null;
                        }
                    }
                    else
                    {
                        conditionSince = null;
                    }
                }
                else if (rule.ClearCondition(s.Value))
                {
                    current!.ClearedAt = s.TimestampUtc;
                    active = false;
                }
            }
        }
        return events;
    }
}

/// <summary>
/// 历史回放（twin-tools S1）：as-of 时间查询——T 时刻值 = 最后一个
/// 时间戳 ≤ T 的样本；无样本 = Unknown（间隙可见）。实时/历史模式经由
/// asOfUtc 是否为 null 区分（实时走 store.Latest 的墙钟质量评估）。
/// </summary>
public sealed class HistoryPlayback
{
    private readonly string _deviceId;
    private readonly Dictionary<(string, string), List<DeviceSample>> _series = new();

    public HistoryPlayback(DeviceDataStore store, string deviceId,
        IReadOnlyList<string> signals, DateTime fromUtc, DateTime toUtc,
        double nominalPeriodSeconds)
    {
        _deviceId = deviceId;
        foreach (var s in signals)
        {
            var (samples, _) = store.Window(deviceId, s, fromUtc, toUtc, nominalPeriodSeconds);
            _series[(deviceId, s)] = samples;   // Window 已升序
        }
    }

    /// <summary>as-of 查询：T 时刻某信号的值与来源时间戳；无 = null。</summary>
    public (double value, DateTime atUtc)? ValueAt(string signal, DateTime asOfUtc)
    {
        if (!_series.TryGetValue((_deviceId, signal), out var list) || list.Count == 0) return null;
        // 二分：最后一个时间戳 ≤ asOfUtc
        int lo = 0, hi = list.Count - 1, ans = -1;
        while (lo <= hi)
        {
            var mid = (lo + hi) / 2;
            if (list[mid].TimestampUtc <= asOfUtc) { ans = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return ans < 0 ? null : (list[ans].Value, list[ans].TimestampUtc);
    }
}
