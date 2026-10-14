using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace PrimalEditor_Avalonia.Input;

/// <summary>键名（S2 数据层用规范名；设备适配是后续阶段）。</summary>
public readonly record struct InputKey(string Name) : IComparable<InputKey>
{
    public int CompareTo(InputKey other) => string.CompareOrdinal(Name, other.Name);
}

/// <summary>动作绑定：动作名 → 键集合（一键可绑多动作=冲突可检）。</summary>
public sealed class ActionBinding
{
    public string Action { get; init; } = "";
    public HashSet<string> Keys { get; } = new(StringComparer.Ordinal);
}

/// <summary>
/// 输入重绑定模型（input-camera S2 数据层，2026-10-15）：加载/解析/重绑/
/// 冲突检测/死区/恢复默认。纯数据 + 纯函数——UI（重绑面板）是消费方。
/// 映射文件契约（与 Player input.bindings 同格式）：
///   # primal-input-bindings v1
///   action MoveForward W UpArrow
/// </summary>
public sealed class InputBindings
{
    public const string FileHeader = "# primal-input-bindings v1";

    /// <summary>动作 → 键集（插入序由 SortedActions 保证输出确定）。</summary>
    private readonly Dictionary<string, HashSet<string>> _map = new(StringComparer.Ordinal);

    /// <summary>模拟量死区（0..1；适用于手柄摇杆键名前缀 "Axis"——S2 记录，
    /// 翻译在宿主设备层）。</summary>
    public Dictionary<string, double> Deadzones { get; } = new(StringComparer.Ordinal);

    public IEnumerable<string> Actions => _map.Keys.OrderBy(a => a, StringComparer.Ordinal);

    public IReadOnlyCollection<string> KeysOf(string action) =>
        _map.TryGetValue(action, out var keys)
            ? keys.OrderBy(k => k, StringComparer.Ordinal).ToList()
            : Array.Empty<string>();

    // ---- 解析/序列化 ----

    /// <summary>解析映射文件文本。'#' 行=注释/头；action 行无有效键=丢弃并计数
    /// （与 Player 同语义）；未知行格式=错误行计数——不静默装成功。</summary>
    public static (InputBindings? bindings, List<string> errors) Parse(string text)
    {
        var result = new InputBindings();
        var errors = new List<string>();
        if (string.IsNullOrWhiteSpace(text))
        {
            errors.Add("映射文件为空");
            return (null, errors);
        }
        var sawHeader = false;
        var lineNo = 0;
        foreach (var raw in text.Split('\n'))
        {
            ++lineNo;
            var line = raw.Trim().TrimEnd('\r');
            if (line.Length == 0) continue;
            if (line.StartsWith('#'))
            {
                if (line.StartsWith("# primal-input-bindings")) sawHeader = true;
                continue;
            }
            var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2 || parts[0] != "action")
            {
                errors.Add($"第 {lineNo} 行格式无效（需 'action <名> <键>…'）");
                continue;
            }
            var action = parts[1];
            var keys = parts.Skip(2).Where(k => k.Length > 0).ToList();
            if (keys.Count == 0)
            {
                errors.Add($"第 {lineNo} 行动作 '{action}' 无有效键——丢弃");
                continue;
            }
            foreach (var k in keys) result.Bind(action, k);
        }
        if (!sawHeader)
            errors.Add("缺少映射文件头（# primal-input-bindings v1）——已按无头容错解析");
        return (result, errors);
    }

    public string Serialize()
    {
        var sb = new StringBuilder();
        sb.AppendLine(FileHeader);
        foreach (var action in Actions)
            sb.AppendLine($"action {action} {string.Join(' ', KeysOf(action))}");
        foreach (var (axis, dz) in Deadzones.OrderBy(kv => kv.Key, StringComparer.Ordinal))
            sb.AppendLine($"deadzone {axis} {dz.ToString("R", System.Globalization.CultureInfo.InvariantCulture)}");
        return sb.ToString();
    }

    // ---- 绑定操作 ----

    /// <summary>绑键（幂等：重复同键无效果）。</summary>
    public void Bind(string action, string key)
    {
        if (string.IsNullOrEmpty(action) || string.IsNullOrEmpty(key)) return;
        if (!_map.TryGetValue(action, out var keys))
        {
            keys = new HashSet<string>(StringComparer.Ordinal);
            _map[action] = keys;
        }
        keys.Add(key);
    }

    /// <summary>解绑（幂等）。动作空集后动作移除。</summary>
    public void Unbind(string action, string key)
    {
        if (!_map.TryGetValue(action, out var keys)) return;
        keys.Remove(key);
        if (keys.Count == 0) _map.Remove(action);
    }

    /// <summary>重绑：把 key 从 from 动作移到 to 动作（无 from 记录=纯新增）。
    /// 无条件执行——冲突检测由调用方先询 ConflictsFor。</summary>
    public void Rebind(string to, string key, string? from = null)
    {
        if (from != null) Unbind(from, key);
        Bind(to, key);
    }

    // ---- 冲突检测（S2 合同：冲突可检）----

    public sealed class Conflict
    {
        public string Key = "";
        public List<string> Actions = new();   // 同键多动作（Ordinal 排序）
    }

    /// <summary>全部冲突：一键绑到多个动作。返回 Ordinal 确定性清单。</summary>
    public List<Conflict> Conflicts() =>
        _map.SelectMany(kv => kv.Value.Select(key => (action: kv.Key, key)))
            .GroupBy(x => x.key, StringComparer.Ordinal)
            .Where(g => g.Count() > 1)
            .Select(g => new Conflict
            {
                Key = g.Key,
                Actions = g.Select(x => x.action).Distinct().OrderBy(a => a, StringComparer.Ordinal).ToList(),
            })
            .OrderBy(c => c.Key, StringComparer.Ordinal)
            .ToList();

    /// <summary>预检：把 key 绑到 to 会产生的新冲突（重绑面板预览用）。</summary>
    public List<Conflict> ConflictsFor(string to, string key)
    {
        var owners = _map
            .Where(kv => kv.Value.Contains(key) && kv.Key != to)
            .Select(kv => kv.Key)
            .ToList();
        return owners.Count == 0
            ? new List<Conflict>()
            : new List<Conflict> { new() { Key = key, Actions = owners.OrderBy(a => a, StringComparer.Ordinal).Append(to).OrderBy(a => a, StringComparer.Ordinal).ToList() } };
    }

    // ---- 死区 ----

    /// <summary>设置轴死区（0..1；越界拒绝）。</summary>
    public bool SetDeadzone(string axis, double value)
    {
        if (value < 0 || value >= 1 || !double.IsFinite(value)) return false;
        Deadzones[axis] = value;
        return true;
    }

    // ---- 默认与合并 ----

    public static InputBindings DefaultPlayer() =>
        Parse(
            FileHeader + "\n" +
            "action MoveForward W UpArrow\n" +
            "action MoveBack S\n" +
            "action MoveLeft A\n" +
            "action MoveRight D\n" +
            "action MoveUp E\n" +
            "action MoveDown Q\n" +
            "action Fast Shift\n").bindings!;

    /// <summary>恢复默认：整体替换（A02 语义：一键回默认，不留半态）。</summary>
    public void ResetToDefault() 
    {
        _map.Clear();
        Deadzones.Clear();
        foreach (var a in DefaultPlayer()._map)
            foreach (var k in a.Value)
                Bind(a.Key, k);
    }
}
