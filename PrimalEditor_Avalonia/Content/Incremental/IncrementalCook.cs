using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content.Incremental;

/// <summary>单条缓存记录（asset-pipeline S2，2026-10-15）。</summary>
public sealed class CookCacheEntry
{
    /// <summary>资产相对路径（缓存键主体）。</summary>
    [JsonPropertyName("assetPath")] public string AssetPath { get; set; } = "";

    /// <summary>输入指纹：源文件内容哈希 + 依赖哈希 + 参数（调用方合成）。</summary>
    [JsonPropertyName("inputHash")] public string InputHash { get; set; } = "";

    /// <summary>转换工具版本（版本变化 = 全量重建该条目）。</summary>
    [JsonPropertyName("toolVersion")] public string ToolVersion { get; set; } = "";

    /// <summary>产物相对路径（验证存在性的依据）。</summary>
    [JsonPropertyName("outputPath")] public string OutputPath { get; set; } = "";

    /// <summary>烘焙时刻（UTC ISO；诊断/展示用）。</summary>
    [JsonPropertyName("cookedAtUtc")] public DateTime CookedAtUtc { get; set; }
}

/// <summary>缓存清单（schemaVersion 未知拒绝，不静默覆写）。</summary>
public sealed class CookCacheManifest
{
    public const int CurrentSchemaVersion = 2;

    [JsonPropertyName("schemaVersion")] public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    [JsonPropertyName("entries")] public List<CookCacheEntry> Entries { get; set; } = new();

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static (CookCacheManifest? manifest, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<CookCacheManifest>(json, JsonOptions);
            if (doc == null) return (null, "缓存清单反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写（迁移：删除缓存重建）");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"缓存清单 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>资产当前输入描述（规划器输入）。</summary>
public sealed class AssetInput
{
    public string AssetPath { get; init; } = "";
    /// <summary>当前输入指纹（同 CookCacheEntry.InputHash 口径）。</summary>
    public string InputHash { get; init; } = "";
    /// <summary>期望产物路径。</summary>
    public string OutputPath { get; init; } = "";
}

/// <summary>重建原因（可解释——"只重建应变项"的应变要能说出来）。</summary>
public enum RebuildReason
{
    New = 0,            // 缓存无记录
    Changed = 1,        // 输入指纹变化（源/依赖/参数）
    ToolVersion = 2,    // 转换版本变化
    MissingOutput = 3,  // 产物文件丢失（outputExists=false）
    Corrupted = 4,      // 缓存条目结构损坏（空哈希/空路径）→ 防伪绿
}

public sealed class CookPlanItem
{
    public string AssetPath { get; init; } = "";
    public RebuildReason Reason { get; init; }
}

/// <summary>增量烘焙计划。</summary>
public sealed class CookPlan
{
    public List<CookPlanItem> ToRebuild { get; } = new();
    public List<string> UpToDate { get; } = new();
    /// <summary>缓存里有、当前输入没有 → 已移除（诊断，不误重建）。</summary>
    public List<string> Removed { get; } = new();
}

/// <summary>
/// 增量规划器（纯函数）：比较当前输入与缓存清单 → 只重建应变项。
/// 判定序（每资产）：无记录→New；指纹变→Changed；工具版本变→ToolVersion；
/// 产物缺失→MissingOutput；条目损坏（空指纹/空产物路径）→Corrupted。
/// 输出确定性排序（AssetPath Ordinal）。
/// </summary>
public static class IncrementalCookPlanner
{
    public static CookPlan Plan(
        IReadOnlyList<AssetInput> inputs,
        CookCacheManifest cache,
        string currentToolVersion,
        Func<string, bool>? outputExists = null)
    {
        var plan = new CookPlan();
        outputExists ??= _ => true;
        var byPath = cache.Entries
            .GroupBy(e => e.AssetPath, StringComparer.Ordinal)
            .ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);
        var currentPaths = new HashSet<string>(StringComparer.Ordinal);

        foreach (var input in inputs)
        {
            currentPaths.Add(input.AssetPath);
            if (!byPath.TryGetValue(input.AssetPath, out var entry))
            {
                plan.ToRebuild.Add(new CookPlanItem { AssetPath = input.AssetPath, Reason = RebuildReason.New });
                continue;
            }
            if (string.IsNullOrEmpty(entry.InputHash) || string.IsNullOrEmpty(entry.OutputPath))
            {
                plan.ToRebuild.Add(new CookPlanItem { AssetPath = input.AssetPath, Reason = RebuildReason.Corrupted });
                continue;
            }
            if (!string.Equals(entry.InputHash, input.InputHash, StringComparison.Ordinal))
            {
                plan.ToRebuild.Add(new CookPlanItem { AssetPath = input.AssetPath, Reason = RebuildReason.Changed });
                continue;
            }
            if (!string.Equals(entry.ToolVersion, currentToolVersion, StringComparison.Ordinal))
            {
                plan.ToRebuild.Add(new CookPlanItem { AssetPath = input.AssetPath, Reason = RebuildReason.ToolVersion });
                continue;
            }
            if (!outputExists(entry.OutputPath))
            {
                plan.ToRebuild.Add(new CookPlanItem { AssetPath = input.AssetPath, Reason = RebuildReason.MissingOutput });
                continue;
            }
            plan.UpToDate.Add(input.AssetPath);
        }

        foreach (var path in byPath.Keys.Where(p => !currentPaths.Contains(p)))
            plan.Removed.Add(path);

        plan.ToRebuild.Sort((a, b) => string.CompareOrdinal(a.AssetPath, b.AssetPath));
        plan.UpToDate.Sort(StringComparer.Ordinal);
        plan.Removed.Sort(StringComparer.Ordinal);
        return plan;
    }

    /// <summary>烘焙成功后写回缓存（增量更新单条；失败资产不写=下次仍重建）。</summary>
    public static void RecordSuccess(CookCacheManifest cache, AssetInput input,
        string toolVersion, DateTime atUtc)
    {
        var existing = cache.Entries.FirstOrDefault(
            e => string.Equals(e.AssetPath, input.AssetPath, StringComparison.Ordinal));
        if (existing != null)
        {
            existing.InputHash = input.InputHash;
            existing.ToolVersion = toolVersion;
            existing.OutputPath = input.OutputPath;
            existing.CookedAtUtc = atUtc;
            return;
        }
        cache.Entries.Add(new CookCacheEntry
        {
            AssetPath = input.AssetPath,
            InputHash = input.InputHash,
            ToolVersion = toolVersion,
            OutputPath = input.OutputPath,
            CookedAtUtc = atUtc,
        });
    }
}
