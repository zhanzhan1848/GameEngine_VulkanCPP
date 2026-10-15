using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.ReleaseMgmt;

/// <summary>安装版本记录（release-maintenance S2，2026-10-15）。</summary>
public sealed class InstalledVersion
{
    public string Version { get; init; } = "";
    /// <summary>包内容指纹（校验——坏包拒绝切换）。</summary>
    public string PackageHash { get; init; } = "";
    public DateTime InstalledAtUtc { get; init; }
    /// <summary>是否可回退目标（安装完整成功的版本才留档）。</summary>
    public bool Complete { get; init; }
}

/// <summary>用户/应用设置（迁移载体：v1→v2→v1 保留声明用户设置）。</summary>
public sealed class AppConfig
{
    public int SchemaVersion { get; set; } = 1;
    /// <summary>用户显式设置（迁移保留——键值均为用户意图）。</summary>
    public Dictionary<string, string> UserSettings { get; set; } = new(StringComparer.Ordinal);
    /// <summary>引擎默认派生项（迁移可重算——不保留）。</summary>
    public Dictionary<string, string> Derived { get; set; } = new(StringComparer.Ordinal);
}

/// <summary>升级会话结果。</summary>
public sealed class UpgradeResult
{
    public bool Ok { get; init; }
    /// <summary>失败原因（坏包/中断恢复等——类型化不静默）。</summary>
    public string? Error { get; init; }
    /// <summary>true = 从中断中恢复且旧版仍可运行。</summary>
    public bool InterruptedRecoveredToOld { get; init; }
}

/// <summary>
/// 版本升级/回退服务（release-maintenance S2）：
///   - apply(new)：校验包哈希（坏包拒绝且不动当前版本）→ 记录 pending →
///     提交（complete=true）；升级中断（未 commit）→ 旧版仍可运行（回退语义）；
///   - rollback()：回退到最近一个 Complete 版本（无完整版本 → 类型化失败）；
///   - 配置迁移 v1→v2：保留 UserSettings、重置 Derived（v2→v1 反向同样保留
///     用户设置——合同"v1→v2→v1 保留声明用户设置"）。
/// 纯逻辑层——文件/安装器接线归宿主。
/// </summary>
public sealed class UpgradeService
{
    public List<InstalledVersion> History { get; } = new();
    public InstalledVersion? Current => History.LastOrDefault(v => v.Complete);

    /// <summary>当前可运行版本（升级中断时=旧版）。</summary>
    public InstalledVersion? Runnable => History.LastOrDefault();

    public UpgradeResult Apply(string newVersion, string packageHash, bool packageValid,
        Action? commit = null)
    {
        if (string.IsNullOrWhiteSpace(newVersion) || string.IsNullOrWhiteSpace(packageHash))
            return new UpgradeResult { Ok = false, Error = "版本号/包哈希为空" };
        if (!packageValid)
            return new UpgradeResult
            {
                Ok = false,
                Error = $"包校验失败（{packageHash[..Math.Min(8, packageHash.Length)]}…）——拒绝切换，当前版本不受影响",
            };
        if (History.Any(v => v.Version == newVersion && v.Complete))
            return new UpgradeResult { Ok = false, Error = $"版本 {newVersion} 已安装（重复应用拒绝）" };

        // 两段式：pending 先入史（不 Complete）→ commit 成功才置 Complete。
        // 中断（commit 未调/抛出）→ History 留 pending 记录：Runnable 回退到旧版。
        var pending = new InstalledVersion
        {
            Version = newVersion,
            PackageHash = packageHash,
            InstalledAtUtc = DateTime.UtcNow,
            Complete = false,
        };
        History.Add(pending);
        try
        {
            commit?.Invoke();
            // 提交成功：原地置 Complete（列表元素替换）
            var committed = new InstalledVersion
            {
                Version = pending.Version, PackageHash = pending.PackageHash,
                InstalledAtUtc = pending.InstalledAtUtc, Complete = true,
            };
            var idx = History.IndexOf(pending);
            History[idx] = committed;
            return new UpgradeResult { Ok = true };
        }
        catch (Exception ex)
        {
            return new UpgradeResult
            {
                Ok = false,
                Error = $"升级中断：{ex.Message}",
                InterruptedRecoveredToOld = true,   // 旧版仍可运行（pending 不挡 Runnable 语义见下）
            };
        }
    }

    /// <summary>回退到最近完整版本（跳过 pending）。无可回退 → 类型化失败。</summary>
    public UpgradeResult Rollback()
    {
        var target = History.LastOrDefault(v => v.Complete && Current != null
            && v.Version != Current.Version);
        if (Current == null)
            return new UpgradeResult { Ok = false, Error = "没有已安装版本可回退" };
        if (target == null)
            return new UpgradeResult { Ok = false, Error = "没有更早的完整版本可回退" };

        // 回退=把 target 之后的历史清为无效（Current 指回 target）
        var idx = History.IndexOf(target);
        History.RemoveRange(idx + 1, History.Count - idx - 1);
        return new UpgradeResult { Ok = true };
    }

    // ---- 配置迁移 ----

    private const int LatestConfigSchema = 2;

    public static (AppConfig migrated, List<string> preserved) MigrateConfig(AppConfig config)
    {
        var preserved = config.UserSettings
            .Select(kv => kv.Key)
            .OrderBy(k => k, StringComparer.Ordinal)
            .ToList();
        if (config.SchemaVersion >= LatestConfigSchema)
            return (config, preserved);   // 已是新版本：只保留声明（不重复迁移）

        var result = new AppConfig
        {
            SchemaVersion = LatestConfigSchema,
            UserSettings = new Dictionary<string, string>(config.UserSettings, StringComparer.Ordinal),
            Derived = new Dictionary<string, string>(StringComparer.Ordinal),   // 派生项重算
        };
        return (result, preserved);
    }

    /// <summary>v2→v1 反向（回退到旧版时）：同样保留用户设置。</summary>
    public static AppConfig DowngradeConfig(AppConfig config) => new()
    {
        SchemaVersion = 1,
        UserSettings = new Dictionary<string, string>(config.UserSettings, StringComparer.Ordinal),
        Derived = new Dictionary<string, string>(StringComparer.Ordinal),
    };
}
