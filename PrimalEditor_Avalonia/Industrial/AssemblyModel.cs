using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Industrial;

/// <summary>属性来源（industrial-models S1，2026-10-15）——源更新语义的依据。</summary>
public enum PropertyOrigin
{
    /// <summary>从外部源文件带入（源更新时会被新值覆盖）。</summary>
    Source = 0,
    /// <summary>引擎侧/用户添加（源更新时保留，不被覆盖）。</summary>
    User = 1,
    /// <summary>引擎内置语义（native——保留）。</summary>
    Native = 2,
}

public sealed class PartProperty
{
    public string Key { get; set; } = "";
    public string Value { get; set; } = "";
    /// <summary>单位（空 = 无量纲/文本）。</summary>
    public string? Unit { get; set; }
    public PropertyOrigin Origin { get; set; } = PropertyOrigin.Source;
}

/// <summary>
/// 装配部件：稳定业务 PartId（跨源重导入保持——身份可追溯）+
/// 源引用（文件 + 源内部路径，如 "pump.step#body"）。
/// </summary>
public sealed class AssemblyPart
{
    public string PartId { get; set; } = "";
    public string Name { get; set; } = "";
    /// <summary>源文件引用（路径）。根装配可为空。</summary>
    public string? SourceFile { get; set; }
    /// <summary>源内部路径/实体名（配合 SourceFile 定位源内部对象）。</summary>
    public string? SourcePath { get; set; }
    public List<PartProperty> Properties { get; set; } = new();
    public List<AssemblyPart> Children { get; set; } = new();

    /// <summary>规范化源引用键（SourceFile#SourcePath；均空 = null）。</summary>
    public string? SourceRef =>
        string.IsNullOrEmpty(SourceFile) && string.IsNullOrEmpty(SourcePath)
            ? null
            : $"{SourceFile ?? ""}#{SourcePath ?? ""}";
}

public sealed class AssemblyDocument
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public string Name { get; set; } = "Assembly";
    public List<AssemblyPart> Roots { get; set; } = new();

    /// <summary>全树遍历（深度优先、子序稳定）。</summary>
    public IEnumerable<AssemblyPart> AllParts()
    {
        IEnumerable<AssemblyPart> Walk(AssemblyPart p)
        {
            yield return p;
            foreach (var c in p.Children)
                foreach (var x in Walk(c))
                    yield return x;
        }
        return Roots.SelectMany(Walk);
    }

    /// <summary>校验：PartId 空/重复、SourceRef 重复（同源对象多部件——需人工拆分
    /// 或显式实例化）→ 错误清单（不静默）。</summary>
    public List<string> Validate()
    {
        var errors = new List<string>();
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var refs = new HashSet<string>(StringComparer.Ordinal);
        foreach (var p in AllParts())
        {
            if (string.IsNullOrWhiteSpace(p.PartId))
                errors.Add($"部件 '{p.Name}' 缺 PartId");
            else if (!ids.Add(p.PartId))
                errors.Add($"重复 PartId: {p.PartId}");
            if (p.SourceRef is { } r && !refs.Add(r))
                errors.Add($"重复源引用: {r}（一个源对象被多个部件引用）");
        }
        return errors;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static (AssemblyDocument? doc, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<AssemblyDocument>(json, JsonOptions);
            if (doc == null) return (null, "装配文档反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"装配文档 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>源更新结果（部件身份稳定 + 逐类报告——不静默）。</summary>
public sealed class AssemblyUpdateReport
{
    public List<string> MatchedBySourceRef = new();     // 身份保持（同 PartId）
    public List<string> AddedParts = new();             // 源新增 → 分配新 PartId
    public List<string> MissingParts = new();           // 源已删 → 保留部件并标 Stale
    public HashSet<string> StalePartIds { get; } = new(StringComparer.Ordinal);

    public List<string> UserPropertiesPreserved = new();
    public int SourcePropertiesUpdated;
}

/// <summary>
/// 源更新服务（industrial-models S1）：把"从源文件重新提取的装配"合并进现有
/// 文档。身份规则：SourceRef 命中 → 沿用旧 PartId（绑定/引用不悬垂）；
/// 源属性以新值为准（Origin=Source 覆盖），User/Native 属性保留；
/// 源缺失的部件保留在文档中并标记 Stale（不静默删除）；新增部件分配新 ID。
/// 输出为【新文档】——原文件不被原地改写（子规划"原件保留，失败不静默覆盖"）。
/// </summary>
public static class AssemblyUpdateService
{
    public static (AssemblyDocument updated, AssemblyUpdateReport report) Merge(
        AssemblyDocument existing, AssemblyDocument freshSource, string newPartIdPrefix)
    {
        var report = new AssemblyUpdateReport();
        var byRef = existing.AllParts()
            .Where(p => p.SourceRef != null)
            .GroupBy(p => p.SourceRef!)
            .ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);
        var usedIds = new HashSet<string>(
            existing.AllParts().Select(p => p.PartId), StringComparer.Ordinal);
        var updated = new AssemblyDocument
        {
            Name = existing.Name,
            Roots = new List<AssemblyPart>(existing.Roots),   // 结构沿用现有树
        };

        foreach (var freshRoot in freshSource.Roots)
        {
            MergePart(freshRoot, updated.Roots, parent: null);
        }

        // 既有部件处理：源引用已消失 → 标 Stale 保留（精确判定=现有引用不在 fresh 源中）
        var freshRefs = freshSource.AllParts()
            .Where(p => p.SourceRef != null)
            .Select(p => p.SourceRef!)
            .ToHashSet(StringComparer.Ordinal);
        foreach (var part in updated.AllParts())
        {
            if (part.SourceRef is { } r && !freshRefs.Contains(r))
            {
                report.StalePartIds.Add(part.PartId);
                report.MissingParts.Add($"{part.PartId} ({r})");
            }
        }
        return (updated, report);

        void MergePart(AssemblyPart fresh, List<AssemblyPart> targetChildren, AssemblyPart? parent)
        {
            AssemblyPart? matched = fresh.SourceRef != null && byRef.TryGetValue(fresh.SourceRef, out var m)
                ? m : null;
            if (matched != null)
            {
                report.MatchedBySourceRef.Add(matched.PartId);
                // 源属性更新（覆盖 Origin=Source 同名键）；User/Native 保留
                var preserved = matched.Properties
                    .Where(p => p.Origin != PropertyOrigin.Source)
                    .ToList();
                foreach (var p in preserved)
                    report.UserPropertiesPreserved.Add($"{matched.PartId}:{p.Key}");
                matched.Properties = fresh.Properties
                    .Select(p => new PartProperty
                    {
                        Key = p.Key, Value = p.Value, Unit = p.Unit,
                        Origin = PropertyOrigin.Source,
                    })
                    .Concat(preserved)
                    .OrderBy(p => p.Key, StringComparer.Ordinal)
                    .ToList();
                report.SourcePropertiesUpdated += fresh.Properties.Count;
                matched.Name = fresh.Name;
                // 结构：fresh 的子部件并入 matched.Children（递归同规则）
                foreach (var child in fresh.Children)
                    MergePart(child, matched.Children, matched);
                if (parent != null && !parent.Children.Contains(matched))
                    parent.Children.Add(matched);
                if (parent == null && !targetChildren.Contains(matched))
                    targetChildren.Add(matched);
            }
            else
            {
                // 源新增：分配未占用的新 PartId（前缀 + 源引用哈希短码，确定性）
                var id = AllocId(newPartIdPrefix, fresh.SourceRef ?? fresh.Name, usedIds);
                var added = new AssemblyPart
                {
                    PartId = id,
                    Name = fresh.Name,
                    SourceFile = fresh.SourceFile,
                    SourcePath = fresh.SourcePath,
                    Properties = fresh.Properties
                        .Select(p => new PartProperty
                        {
                            Key = p.Key, Value = p.Value, Unit = p.Unit,
                            Origin = PropertyOrigin.Source,
                        })
                        .ToList(),
                };
                report.AddedParts.Add($"{id} ({fresh.SourceRef ?? fresh.Name})");
                targetChildren.Add(added);
                foreach (var child in fresh.Children)
                    MergePart(child, added.Children, added);
            }
        }
    }

    /// <summary>确定性短码（FNV-1a 64 → 8 hex）+ 冲突递增。</summary>
    private static string AllocId(string prefix, string seed, HashSet<string> used)
    {
        ulong h = 14695981039346656037ul;
        foreach (var c in seed)
        {
            h ^= c;
            h *= 1099511628211ul;
        }
        var code = h.ToString("x8")[..8];
        var id = $"{prefix}-{code}";
        int n = 1;
        while (used.Contains(id))
            id = $"{prefix}-{code}-{n++}";
        used.Add(id);
        return id;
    }
}
