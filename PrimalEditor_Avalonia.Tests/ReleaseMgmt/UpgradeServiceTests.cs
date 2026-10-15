using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.ReleaseMgmt;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ReleaseMgmtNs;

/// <summary>
/// release-maintenance S2 契约测试（A01/A02/A03）：
/// v1→v2→v1 保留声明用户设置；模拟更新中断能运行旧版；坏包校验拒绝切换；
/// 回退到上一完整版本；重复应用拒绝；无版本回退类型化失败。
/// </summary>
public sealed class UpgradeServiceTests
{
    [Fact]
    public void ConfigMigration_V1V2V1_PreservesUserSettings()
    {
        var v1 = new AppConfig
        {
            SchemaVersion = 1,
            UserSettings = { ["theme"] = "dark", ["locale"] = "zh-CN" },
            Derived = { ["cache-path"] = "/tmp/x" },
        };

        var (v2, preserved) = UpgradeService.MigrateConfig(v1);
        Assert.Equal(2, v2.SchemaVersion);
        Assert.Equal(new[] { "locale", "theme" }, preserved);   // 声明保留（Ordinal）
        Assert.Equal("dark", v2.UserSettings["theme"]);          // 用户设置保留
        Assert.Empty(v2.Derived);                                 // 派生项重算（不保留）

        // 回退 v2→v1：用户设置仍保留
        var back = UpgradeService.DowngradeConfig(v2);
        Assert.Equal(1, back.SchemaVersion);
        Assert.Equal("zh-CN", back.UserSettings["locale"]);

        // 已是新版本再迁移=幂等
        var (again, _) = UpgradeService.MigrateConfig(v2);
        Assert.Equal(2, again.SchemaVersion);
        Assert.Equal("dark", again.UserSettings["theme"]);
    }

    [Fact]
    public void BadPackage_Rejected_CurrentUnaffected()
    {
        var svc = new UpgradeService();
        var r1 = svc.Apply("1.0.0", "hash-1.0", packageValid: true, commit: () => { });
        Assert.True(r1.Ok);

        var bad = svc.Apply("2.0.0", "corrupted-hash", packageValid: false);
        Assert.False(bad.Ok);
        Assert.Contains("包校验失败", bad.Error);
        Assert.Equal("1.0.0", svc.Current!.Version);   // 当前版本不受影响
        Assert.Single(svc.History);                     // 坏包不入史
    }

    [Fact]
    public void InterruptedUpgrade_OldVersionStillRunnable()
    {
        var svc = new UpgradeService();
        svc.Apply("1.0.0", "hash-1.0", true, commit: () => { });

        // 模拟更新中断：commit 抛出（安装过程崩溃/断电）
        var interrupted = svc.Apply("2.0.0", "hash-2.0", true,
            commit: () => throw new InvalidOperationException("power loss"));
        Assert.False(interrupted.Ok);
        Assert.True(interrupted.InterruptedRecoveredToOld);
        Assert.Equal("1.0.0", svc.Current!.Version);    // 运行版本仍是旧版

        // 重试升级成功
        var retry = svc.Apply("2.0.0", "hash-2.0-fixed", true, commit: () => { });
        Assert.True(retry.Ok);
        Assert.Equal("2.0.0", svc.Current.Version);
    }

    [Fact]
    public void Rollback_ReturnsToPreviousCompleteVersion()
    {
        var svc = new UpgradeService();
        svc.Apply("1.0.0", "h1", true, commit: () => { });
        svc.Apply("2.0.0", "h2", true, commit: () => { });
        Assert.Equal("2.0.0", svc.Current!.Version);

        var rb = svc.Rollback();
        Assert.True(rb.Ok);
        Assert.Equal("1.0.0", svc.Current.Version);     // 回到上一完整版

        // 再回退：没有更早版本 → 类型化失败
        var rb2 = svc.Rollback();
        Assert.False(rb2.Ok);
        Assert.Contains("没有更早", rb2.Error);
    }

    [Fact]
    public void DuplicateApply_AndEmptyVersion_Rejected()
    {
        var svc = new UpgradeService();
        svc.Apply("1.0.0", "h1", true, commit: () => { });
        var dup = svc.Apply("1.0.0", "h1", true, commit: () => { });
        Assert.False(dup.Ok);
        Assert.Contains("已安装", dup.Error);

        var empty = svc.Apply("", "h", true);
        Assert.False(empty.Ok);
        Assert.Contains("为空", empty.Error);

        var fresh = new UpgradeService();
        var none = fresh.Rollback();
        Assert.False(none.Ok);
        Assert.Contains("没有已安装", none.Error);
    }
}
