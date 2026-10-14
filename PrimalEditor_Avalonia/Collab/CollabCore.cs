using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace PrimalEditor_Avalonia.Collab;

/// <summary>
/// 跨资产事务参与者（editor-collaboration S1，2026-10-15）：文档的
/// （预期前像, 目标后像）。前像匹配 = 乐观并发（协作冲突检测的依据）。
/// </summary>
public sealed class DocumentChange
{
    public string DocId { get; init; } = "";
    /// <summary>变更前 JSON（调用方从文档当前状态序列化）。</summary>
    public string BeforeJson { get; init; } = "";
    /// <summary>变更后 JSON。</summary>
    public string AfterJson { get; init; } = "";
}

/// <summary>事务结果：全部成功或全部不动（all-or-nothing），冲突逐文档报告。</summary>
public sealed class TransactionResult
{
    public bool Applied { get; init; }
    /// <summary>前像不匹配的文档（已被他方修改）→ 冲突清单（不静默覆盖）。</summary>
    public List<string> ConflictedDocIds { get; init; } = new();
}

/// <summary>
/// 跨资产事务（editor-collaboration S1）：多文档原子应用。读当前值经注入的
/// 读取器（不绑具体存储——编辑器文档/资产注册表/测试均可接入）；写经注入的
/// 写入器。验证所有前像匹配后一次性写入——任一冲突则零写入。
/// </summary>
public static class CollabTransaction
{
    public static TransactionResult Apply(
        IReadOnlyList<DocumentChange> changes,
        Func<string, string?> readCurrent,
        Action<string, string> write)
    {
        if (changes.GroupBy(c => c.DocId).Any(g => g.Count() > 1))
            throw new InvalidOperationException("事务内同一文档出现多次（请合并为一条变更）");

        var conflicts = new List<string>();
        foreach (var c in changes)
        {
            var current = readCurrent(c.DocId);
            if (current != c.BeforeJson)
                conflicts.Add(c.DocId);   // 已被他方修改（或不存在）——不覆盖
        }
        if (conflicts.Count > 0)
            return new TransactionResult { Applied = false, ConflictedDocIds = conflicts };

        foreach (var c in changes)
            write(c.DocId, c.AfterJson);
        return new TransactionResult { Applied = true };
    }

    /// <summary>逆应用（事务级 Undo）：交换前后像、同样前像匹配（后像=当前）。</summary>
    public static TransactionResult Revert(
        IReadOnlyList<DocumentChange> changes,
        Func<string, string?> readCurrent,
        Action<string, string> write) =>
        Apply(changes.Select(c => new DocumentChange
        {
            DocId = c.DocId,
            BeforeJson = c.AfterJson,
            AfterJson = c.BeforeJson,
        }).ToList(), readCurrent, write);
}

/// <summary>语义差异条目（路径 + 增/删/改）。</summary>
public sealed class SemanticChange
{
    /// <summary>"add" / "remove" / "change"。</summary>
    public string Kind { get; init; } = "";
    /// <summary>属性路径（a.b.c；数组整体报告为所在路径）。</summary>
    public string Path { get; init; } = "";
    public JsonElement? Before { get; init; }
    public JsonElement? After { get; init; }
}

/// <summary>
/// 语义差异（对象树逐属性比较；数组按值整体报告——元素级差异是后续阶段）。
/// 排序确定性（路径 Ordinal）。
/// </summary>
public static class SemanticDiff
{
    public static List<SemanticChange> Between(string beforeJson, string afterJson)
    {
        using var before = JsonDocument.Parse(beforeJson ?? "null");
        using var after = JsonDocument.Parse(afterJson ?? "null");
        var changes = new List<SemanticChange>();
        Walk(before.RootElement, after.RootElement, "", changes);
        changes.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
        return changes;
    }

    private static void Walk(JsonElement b, JsonElement a, string path, List<SemanticChange> changes)
    {
        if (b.ValueKind == JsonValueKind.Object && a.ValueKind == JsonValueKind.Object)
        {
            foreach (var prop in b.EnumerateObject())
            {
                var child = path.Length == 0 ? prop.Name : $"{path}.{prop.Name}";
                if (a.TryGetProperty(prop.Name, out var afterChild))
                    Walk(prop.Value, afterChild, child, changes);
                else
                    changes.Add(new SemanticChange { Kind = "remove", Path = child, Before = Clone(prop.Value) });
            }
            foreach (var prop in a.EnumerateObject())
            {
                if (!b.TryGetProperty(prop.Name, out _))
                {
                    var child = path.Length == 0 ? prop.Name : $"{path}.{prop.Name}";
                    changes.Add(new SemanticChange { Kind = "add", Path = child, After = Clone(prop.Value) });
                }
            }
            return;
        }
        if (b.ValueKind == JsonValueKind.Array && a.ValueKind == JsonValueKind.Array)
        {
            if (b.GetArrayLength() != a.GetArrayLength() || !JsonEqual(b, a))
                changes.Add(new SemanticChange { Kind = "change", Path = path, Before = Clone(b), After = Clone(a) });
            return;
        }
        if (!JsonEqual(b, a))
            changes.Add(new SemanticChange { Kind = "change", Path = path, Before = Clone(b), After = Clone(a) });
    }

    private static bool JsonEqual(JsonElement x, JsonElement y) =>
        x.GetRawText() == y.GetRawText() ||
        (x.ValueKind == JsonValueKind.Number && y.ValueKind == JsonValueKind.Number
            && x.GetDouble() == y.GetDouble());

    private static JsonElement? Clone(JsonElement e) =>
        JsonSerializer.Deserialize<JsonElement>(e.GetRawText());
}
