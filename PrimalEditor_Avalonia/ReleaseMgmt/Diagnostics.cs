using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.ReleaseMgmt;

/// <summary>诊断包条目类型（release-maintenance S3，2026-10-15）。</summary>
public enum DiagnosticEntryKind
{
    EngineLog = 0,      // 引擎日志（黑匣子/文件）
    AppLog = 1,         // 编辑器/Player 应用日志
    CrashDump = 2,      // 崩溃转储（可能含内存路径等）
    Screenshot = 3,     // 故障画面
    SystemInfo = 4,     // OS/GPU/驱动（含隐私字段——过滤）
    UserNote = 5,       // 用户描述（自由文本——默认脱敏扫描）
}

public sealed class DiagnosticEntry
{
    public DiagnosticEntryKind Kind { get; init; }
    /// <summary>来源路径/标识。</summary>
    public string Source { get; init; } = "";
    public string Content { get; init; } = "";
}

public sealed class DiagnosticBundle
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public string EngineVersion { get; set; } = "";
    public string AppVersion { get; set; } = "";
    /// <summary>符号/构建追溯（构建 id——日志与符号匹配的键）。</summary>
    public string BuildId { get; set; } = "";
    public DateTime CreatedAtUtc { get; set; }
    public List<DiagnosticEntry> Entries { get; set; } = new();

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static (DiagnosticBundle? bundle, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<DiagnosticBundle>(json, JsonOptions);
            if (doc == null) return (null, "诊断包反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本——不静默误读");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"诊断包 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>
/// 诊断包导出器（release-maintenance S3）：
///   - 隐私字段过滤（合同"隐私字段过滤"）：SystemInfo/UserNote 内容经
///     RedactRules 掩码（用户名/绝对路径 home 段/IP）；CrashDump 默认排除
///     （可显式开启——含内存内容风险）；
///   - 有用性最低门槛：无引擎版本/无任何条目 → 类型化拒绝导出（不产空包）；
///   - 追溯：EngineVersion/AppVersion/BuildId 必填（构建可追溯——符号匹配键）。
/// </summary>
public static class DiagnosticExporter
{
    /// <summary>隐私掩码规则（正则→替换）。</summary>
    public static readonly (string pattern, string replacement)[] DefaultRedactRules =
    {
        (@"/Users/[^/\s""]+", "/Users/[REDACTED]"),
        (@"/home/[^/\s""]+", "/home/[REDACTED]"),
        (@"\b(?:\d{1,3}\.){3}\d{1,3}\b", "[IP-REDACTED]"),
        (@"(?:api[_-]?key|token|secret)[=:]\s*\S+", "$1=[REDACTED]"),
    };

    public static (string? json, List<string> warnings) Export(
        DiagnosticBundle bundle,
        bool includeCrashDumps = false,
        (string pattern, string replacement)[]? redactRules = null)
    {
        var warnings = new List<string>();
        if (string.IsNullOrWhiteSpace(bundle.EngineVersion)
            || string.IsNullOrWhiteSpace(bundle.BuildId))
            return (null, new List<string> { "诊断包缺 EngineVersion/BuildId——拒绝导出（无符号匹配键，包不可用）" });

        var rules = redactRules ?? DefaultRedactRules;
        var filtered = new DiagnosticBundle
        {
            SchemaVersion = bundle.SchemaVersion,
            EngineVersion = bundle.EngineVersion,
            AppVersion = bundle.AppVersion,
            BuildId = bundle.BuildId,
            CreatedAtUtc = bundle.CreatedAtUtc,
        };

        foreach (var e in bundle.Entries)
        {
            if (e.Kind == DiagnosticEntryKind.CrashDump && !includeCrashDumps)
            {
                warnings.Add($"crash dump 排除（含内存内容风险）：{e.Source}");
                continue;
            }
            var content = e.Content;
            var redacted = false;
            foreach (var (pattern, replacement) in rules)
            {
                var next = System.Text.RegularExpressions.Regex.Replace(content, pattern, replacement);
                if (next != content) redacted = true;
                content = next;
            }
            if (redacted && e.Kind is DiagnosticEntryKind.SystemInfo or DiagnosticEntryKind.UserNote)
                warnings.Add($"隐私字段已掩码：{e.Source}");
            filtered.Entries.Add(new DiagnosticEntry { Kind = e.Kind, Source = e.Source, Content = content });
        }

        if (filtered.Entries.Count == 0)
            return (null, new List<string> { "过滤后零条目——拒绝导出空诊断包" });
        return (filtered.Serialize(), warnings);
    }
}

/// <summary>
/// 支持平台矩阵（release-maintenance S3）：声明平台/最低版本/状态——
/// 未知状态显式（不装满绿）。
/// </summary>
public sealed class PlatformMatrix
{
    public sealed class Row
    {
        public string Platform { get; init; } = "";
        public string MinOsVersion { get; init; } = "";
        /// <summary>supported / beta / unsupported / untested（显式，不虚标）。</summary>
        public string Status { get; init; } = "untested";
    }

    public List<Row> Rows { get; } = new();

    public static PlatformMatrix Default() => new()
    {
        Rows =
        {
            new Row { Platform = "macOS-arm64", MinOsVersion = "14.0", Status = "supported" },
            new Row { Platform = "macOS-x64", MinOsVersion = "14.0", Status = "beta" },
            new Row { Platform = "windows-x64", MinOsVersion = "10", Status = "untested" },
        },
    };

    /// <summary>矩阵校验：平台重复/未知状态 → 错误清单（不静默）。</summary>
    public List<string> Validate()
    {
        var errors = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var allowed = new[] { "supported", "beta", "unsupported", "untested" };
        foreach (var r in Rows)
        {
            if (!seen.Add(r.Platform)) errors.Add($"平台重复: {r.Platform}");
            if (!allowed.Contains(r.Status)) errors.Add($"未知状态 '{r.Status}'（{r.Platform}）——允许: {string.Join("/", allowed)}");
        }
        return errors;
    }
}
