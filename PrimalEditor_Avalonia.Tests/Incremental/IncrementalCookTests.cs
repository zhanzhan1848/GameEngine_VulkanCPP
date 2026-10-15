using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Content.Incremental;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.IncrementalNs;

/// <summary>
/// asset-pipeline S2 增量/迁移契约测试：只重建应变项（新/变更/版本/产物缺失/
/// 损坏各原因可解释）、未变项零重建、移除项诊断不误重建、成功写回增量更新、
/// 失败不写回（下次仍重建）、清单往返+未知 schemaVersion 拒绝（迁移语义）。
/// </summary>
public sealed class IncrementalCookTests
{
    private static readonly AssetInput[] Inputs = new[]
    {
        new AssetInput { AssetPath = "a.mesh", InputHash = "h1", OutputPath = "cooked/a" },
        new AssetInput { AssetPath = "b.mesh", InputHash = "h2", OutputPath = "cooked/b" },
        new AssetInput { AssetPath = "c.mesh", InputHash = "h3", OutputPath = "cooked/c" },
    };

    private static CookCacheManifest FilledCache()
    {
        var cache = new CookCacheManifest();
        foreach (var i in Inputs)
            IncrementalCookPlanner.RecordSuccess(cache, i, "v1", DateTime.UtcNow);
        return cache;
    }

    [Fact]
    public void AllUpToDate_WhenNothingChanged()
    {
        var plan = IncrementalCookPlanner.Plan(Inputs, FilledCache(), "v1");
        Assert.Empty(plan.ToRebuild);
        Assert.Equal(new[] { "a.mesh", "b.mesh", "c.mesh" }, plan.UpToDate);
        Assert.Empty(plan.Removed);
    }

    [Fact]
    public void OnlyChangedAssetsRebuild_WithReasons()
    {
        var cache = FilledCache();
        // b 源文件变化；d 是新资产
        var inputs = new[]
        {
            Inputs[0],
            new AssetInput { AssetPath = "b.mesh", InputHash = "h2-changed", OutputPath = "cooked/b" },
            Inputs[2],
            new AssetInput { AssetPath = "d.mesh", InputHash = "h4", OutputPath = "cooked/d" },
        };
        var plan = IncrementalCookPlanner.Plan(inputs, cache, "v1");
        Assert.Equal(2, plan.ToRebuild.Count);
        Assert.Contains(plan.ToRebuild, i => i.AssetPath == "b.mesh" && i.Reason == RebuildReason.Changed);
        Assert.Contains(plan.ToRebuild, i => i.AssetPath == "d.mesh" && i.Reason == RebuildReason.New);
        Assert.Equal(new[] { "a.mesh", "c.mesh" }, plan.UpToDate);   // 只重建应变项
    }

    [Fact]
    public void ToolVersionBump_RebuildsAll_MigrationSemantics()
    {
        var plan = IncrementalCookPlanner.Plan(Inputs, FilledCache(), "v2");
        Assert.Empty(plan.UpToDate);
        Assert.Equal(3, plan.ToRebuild.Count);
        Assert.All(plan.ToRebuild, i => Assert.Equal(RebuildReason.ToolVersion, i.Reason));
    }

    [Fact]
    public void MissingOutput_Detected_NotSilentUpToDate()
    {
        var cache = FilledCache();
        var plan = IncrementalCookPlanner.Plan(
            Inputs, cache, "v1",
            outputExists: path => path != "cooked/c");   // c 产物丢失
        var item = Assert.Single(plan.ToRebuild);
        Assert.Equal(("c.mesh", RebuildReason.MissingOutput), (item.AssetPath, item.Reason));
    }

    [Fact]
    public void CorruptedEntry_Rebuilds_NotFakeGreen()
    {
        var cache = FilledCache();
        cache.Entries[1].InputHash = "";   // b 条目损坏（空指纹）
        var plan = IncrementalCookPlanner.Plan(Inputs, cache, "v1");
        var b = plan.ToRebuild.Single(i => i.AssetPath == "b.mesh");
        Assert.Equal(RebuildReason.Corrupted, b.Reason);
    }

    [Fact]
    public void RemovedAssets_Diagnosed_NotRebuilt()
    {
        var cache = FilledCache();
        var plan = IncrementalCookPlanner.Plan(
            Inputs.Take(2).ToList(), cache, "v1");   // c 不在当前输入
        Assert.Empty(plan.ToRebuild);
        Assert.Equal(new[] { "c.mesh" }, plan.Removed);
        Assert.Equal(new[] { "a.mesh", "b.mesh" }, plan.UpToDate);
    }

    [Fact]
    public void RecordSuccess_UpdatesIncrementally_AndFailureStaysDirty()
    {
        var cache = FilledCache();
        // 变更后烘焙成功 → 写回
        var changed = new AssetInput { AssetPath = "b.mesh", InputHash = "h2-new", OutputPath = "cooked/b" };
        IncrementalCookPlanner.RecordSuccess(cache, changed, "v2", DateTime.UtcNow);
        var entry = cache.Entries.Single(e => e.AssetPath == "b.mesh");
        Assert.Equal("h2-new", entry.InputHash);       // 增量更新（不重复添加）
        Assert.Equal("v2", entry.ToolVersion);
        Assert.Equal(3, cache.Entries.Count);

        // 失败资产不写回：a 变更但模拟失败 → 缓存仍 h1 → 下次仍重建
        var planAfterFailure = IncrementalCookPlanner.Plan(
            new[] { new AssetInput { AssetPath = "a.mesh", InputHash = "h1-new", OutputPath = "cooked/a" } },
            cache, "v2");
        var a = planAfterFailure.ToRebuild.Single(i => i.AssetPath == "a.mesh");
        Assert.Equal(RebuildReason.Changed, a.Reason);   // 缓存没被失败结果污染
    }

    [Fact]
    public void Manifest_RoundTrip_AndFutureSchemaRejected()
    {
        var cache = FilledCache();
        var (loaded, err) = CookCacheManifest.Load(cache.Serialize());
        Assert.Null(err);
        Assert.Equal(3, loaded!.Entries.Count);
        Assert.Equal("h2", loaded.Entries.Single(e => e.AssetPath == "b.mesh").InputHash);

        var (future, futureErr) = CookCacheManifest.Load(
            cache.Serialize().Replace("\"schemaVersion\": 2", "\"schemaVersion\": 99"));
        Assert.Null(future);
        Assert.Contains("不静默覆写", futureErr);   // 迁移语义：明确拒绝而非误读
    }
}
