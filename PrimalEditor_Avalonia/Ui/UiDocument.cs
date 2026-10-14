using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Ui;

/// <summary>UI 节点类型（runtime-ui S1 基础控件集；渲染由宿主实现）。</summary>
public static class UiTypes
{
    public static readonly string[] Supported =
        { "StackPanel", "Text", "Button", "Slider", "Image" };
}

/// <summary>UI 节点：控件树 + 属性 + 绑定声明。</summary>
public sealed class UiNode
{
    public string Id { get; set; } = "";
    /// <summary>控件类型（UiTypes.Supported 之一）。</summary>
    public string Type { get; set; } = "Text";
    /// <summary>直接属性（未绑定时的静态值；字符串形式）。</summary>
    public Dictionary<string, string> Properties { get; set; } = new();
    /// <summary>绑定：UI 属性 ← 数据上下文路径。</summary>
    public List<UiBinding> Bindings { get; set; } = new();
    public List<UiNode> Children { get; set; } = new();
}

/// <summary>绑定声明（OneWay：context → UI；S1 不做反向）。</summary>
public sealed class UiBinding
{
    /// <summary>数据上下文路径（点分层，如 "player.health"）。</summary>
    public string Path { get; set; } = "";
    /// <summary>目标 UI 属性名（如 "text"/"value"/"isVisible"）。</summary>
    public string Property { get; set; } = "";
}

/// <summary>求值后的节点（渲染宿主消费的最终形态；绑定状态显式）。</summary>
public sealed class ResolvedUiNode
{
    public string Id = "";
    public string Type = "";
    /// <summary>属性终值（绑定值或静态值）。</summary>
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
    /// <summary>绑定求值诊断：路径 → "ok"/"missing"（缺失不静默，渲染层可显示占位）。</summary>
    public Dictionary<string, string> BindingStatus { get; } = new(StringComparer.Ordinal);
    public List<ResolvedUiNode> Children { get; } = new();
}

/// <summary>UI 文档（schemaVersion；ID 全树唯一）。</summary>
public sealed class UiDocument
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public string Name { get; set; } = "hud";
    public UiNode Root { get; set; } = new();

    /// <summary>校验：空 ID/重复 ID/未知控件类型 → 错误清单（不静默）。</summary>
    public List<string> Validate()
    {
        var errors = new List<string>();
        var ids = new HashSet<string>(StringComparer.Ordinal);
        void Walk(UiNode n)
        {
            if (string.IsNullOrWhiteSpace(n.Id)) errors.Add($"节点缺 Id（type={n.Type}）");
            else if (!ids.Add(n.Id)) errors.Add($"重复节点 Id: {n.Id}");
            if (!UiTypes.Supported.Contains(n.Type)) errors.Add($"未知控件类型 '{n.Type}'（节点 {n.Id}）");
            foreach (var c in n.Children) Walk(c);
        }
        Walk(Root);
        return errors;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static (UiDocument? doc, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<UiDocument>(json, JsonOptions);
            if (doc == null) return (null, "UI 文档反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"UI 文档 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>
/// 绑定求值器（runtime-ui S1 后端契约）：数据上下文（嵌套字典，叶子为
/// string/double/bool）→ ResolvedUiNode 树。语义：
///   - 绑定命中 → 属性取上下文值（数字/布尔转字符串，文化不敏感）；
///   - 路径缺失 → 属性保持静态值 + BindingStatus["路径"]="missing"（不静默，
///     渲染层可显示占位/告警，不假装绑定成功）；
///   - 绑定优先于静态值（声明绑定即意图绑定）。
/// </summary>
public static class UiEvaluator
{
    public static ResolvedUiNode Evaluate(UiDocument doc, object? context) =>
        EvaluateNode(doc.Root, context);

    private static ResolvedUiNode EvaluateNode(UiNode node, object? context)
    {
        var resolved = new ResolvedUiNode { Id = node.Id, Type = node.Type };
        foreach (var (key, value) in node.Properties)
            resolved.Values[key] = value;                       // 静态值打底
        foreach (var binding in node.Bindings)
        {
            var found = TryResolvePath(context, binding.Path, out var value);
            resolved.BindingStatus[binding.Path] = found ? "ok" : "missing";
            if (found)
                resolved.Values[binding.Property] = value!;
        }
        foreach (var child in node.Children)
            resolved.Children.Add(EvaluateNode(child, context));
        return resolved;
    }

    /// <summary>点分路径解析；叶值统一转字符串（不变文化）。</summary>
    public static bool TryResolvePath(object? context, string path, out string? value)
    {
        value = null;
        if (context == null || string.IsNullOrEmpty(path)) return false;
        object current = context;
        foreach (var segment in path.Split('.'))
        {
            if (current is IReadOnlyDictionary<string, object> dict
                && dict.TryGetValue(segment, out var next))
            {
                current = next;
            }
            else
            {
                return false;
            }
        }
        value = current switch
        {
            string s => s,
            bool b => b ? "true" : "false",
            double d => d.ToString("R", System.Globalization.CultureInfo.InvariantCulture),
            int i => i.ToString(System.Globalization.CultureInfo.InvariantCulture),
            _ => current.ToString(),
        };
        return value != null;
    }
}
