using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Integration;

/// <summary>
/// 集成中心数据契约（software-integrations-and-skills S1，2026-10-15）。
/// 状态语义（总规划 §5）：静态发现与实际 probe 分离——S1 只做"存在性发现"，
/// ProbeFailed/Verified 留给 S2 按需验证。纪律："未扫描到"必须表达为
/// NotFound/Unknown，不得写成"不存在"。
/// </summary>
public enum DiscoveryStatus
{
    /// <summary>尚未扫描（缓存里没有该软件的记录）。</summary>
    Unknown = 0,
    /// <summary>已扫描、所有候选入口与手动路径都未命中。≠ 软件不存在。</summary>
    NotFound = 1,
    /// <summary>静态发现命中（入口文件存在）。可调用性未验证（S2 probe）。</summary>
    Discovered = 2,
    /// <summary>用户显式指定的路径不存在（配置缺失，修复入口 = 重新定位）。</summary>
    ConfigMissing = 3,
    /// <summary>S2：按需 probe 失败（占位——S1 不产生）。</summary>
    ProbeFailed = 4,
    /// <summary>S2：按需 probe 通过（占位——S1 不产生）。</summary>
    Verified = 5,
}

/// <summary>软件记录：稳定 ID + 类别 + 本机/远端 + 候选入口 + 发现结果。</summary>
public sealed class SoftwareRecord
{
    /// <summary>稳定业务 ID（如 "ffmpeg"）——持久引用用它，不用数组位置。</summary>
    [JsonPropertyName("softwareId")] public string SoftwareId { get; set; } = "";
    /// <summary>类别（video-encode / dcc / cloud / slicer / ...）。</summary>
    [JsonPropertyName("category")] public string Category { get; set; } = "";
    /// <summary>true = 远端服务（云集成；S1 只登记，不探测连通）。</summary>
    [JsonPropertyName("isRemote")] public bool IsRemote { get; set; }
    /// <summary>适配器静态候选入口（绝对路径或 PATH 命令名），发现来源可追溯。</summary>
    [JsonPropertyName("candidates")] public List<string> Candidates { get; set; } = new();
    /// <summary>用户手动定位路径（显式选择，检测结果不覆盖它）。</summary>
    [JsonPropertyName("manualPath")] public string? ManualPath { get; set; }
    /// <summary>本轮解析出的入口（manualPath 优先，其次首个存在的候选）。</summary>
    [JsonPropertyName("resolvedPath")] public string? ResolvedPath { get; set; }
    /// <summary>版本（S1 静态发现不启动进程读版本——null = 未知，非空串）。</summary>
    [JsonPropertyName("version")] public string? Version { get; set; }
    /// <summary>发现来源："manual" / "candidate" / "cache" / null。</summary>
    [JsonPropertyName("source")] public string? Source { get; set; }
    [JsonPropertyName("status")] public DiscoveryStatus Status { get; set; } = DiscoveryStatus.Unknown;
    /// <summary>检测时间（UTC ISO-8601；缓存合并按它判新旧）。</summary>
    [JsonPropertyName("detectedAtUtc")] public DateTime DetectedAtUtc { get; set; }
    /// <summary>适配器版本（迁移/失效规则的数据依据）。</summary>
    [JsonPropertyName("adapterVersion")] public int AdapterVersion { get; set; } = 1;
}

/// <summary>Skill 来源作用域。</summary>
public enum SkillOrigin
{
    /// <summary>引擎内置（随编辑器分发）。</summary>
    Internal = 0,
    /// <summary>用户级（~/.primal/skills）。</summary>
    User = 1,
    /// <summary>项目级（&lt;project&gt;/Skills）。</summary>
    Project = 2,
    /// <summary>外部 harness/位置——不可枚举，S1 不产出记录（状态层表达"未知"）。</summary>
    External = 3,
}

/// <summary>Skill 记录（S1 只读清单——不做依赖匹配与验证）。</summary>
public sealed class SkillRecord
{
    [JsonPropertyName("skillId")] public string SkillId { get; set; } = "";
    [JsonPropertyName("description")] public string Description { get; set; } = "";
    [JsonPropertyName("origin")] public SkillOrigin Origin { get; set; }
    /// <summary>清单文件路径（内部/用户/项目来源必有；外部不可枚举）。</summary>
    [JsonPropertyName("path")] public string? Path { get; set; }
    /// <summary>所需 capability 列表（如 ["ffmpeg.encode"]；S1 不做匹配，只登记）。</summary>
    [JsonPropertyName("requires")] public List<string> Requires { get; set; } = new();
    /// <summary>S1 恒 Unknown：有 Skill ≠ 任务已验证（合同）。</summary>
    [JsonPropertyName("lastValidation")] public string LastValidation { get; set; } = "unknown";
}

/// <summary>持久化清单（用户级缓存文档，schemaVersion 拒绝静默覆写未知版本）。</summary>
public sealed class IntegrationInventory
{
    public const int CurrentSchemaVersion = 1;

    [JsonPropertyName("schemaVersion")] public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    [JsonPropertyName("generatedAtUtc")] public DateTime GeneratedAtUtc { get; set; }
    [JsonPropertyName("software")] public List<SoftwareRecord> Software { get; set; } = new();
    [JsonPropertyName("skills")] public List<SkillRecord> Skills { get; set; } = new();

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public static (IntegrationInventory? inventory, string? error) Load(string path)
    {
        try
        {
            if (!File.Exists(path)) return (null, null);   // 无缓存 = 全 Unknown，非错误
            var doc = JsonSerializer.Deserialize<IntegrationInventory>(File.ReadAllText(path), JsonOptions);
            if (doc == null) return (null, "缓存反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"缓存损坏（{ex.Message}）——可用 integrations.list refresh 重建");
        }
        catch (IOException ex)
        {
            return (null, $"缓存读取失败（{ex.Message}）");
        }
    }

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);
}
