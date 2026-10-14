using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Prefabs;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.PrefabNs;

/// <summary>
/// prefab S1 覆盖/传播服务契约测试：
/// 捕获（根 position 排除/显式含根）、还原即清除、Revert 恢复源值、
/// Apply 提升回源文档（.prefab 落盘重载一致）、源更新传播（覆盖保留/其余取源）、
/// 诊断不静默（stale/unmapped/重名）、覆盖字典序列化往返、Undo 整体还原。
/// </summary>
public sealed class PrefabOverrideServiceTests
{
    private static ComponentDocument TransformDoc(float px = 0, float py = 0, float pz = 0,
        float sx = 1, float sy = 1, float sz = 1)
    {
        var t = new Transform
        {
            PositionX = px, PositionY = py, PositionZ = pz,
            ScaleX = sx, ScaleY = sy, ScaleZ = sz,
        };
        return new ComponentDocument
        {
            Type = "Transform",
            Data = JsonSerializer.SerializeToElement(t, new JsonSerializerOptions
            {
                PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            }),
        };
    }

    private static EntityDocument EntityDoc(string name, ComponentDocument transform) => new()
    {
        EntityGuid = EntityGuid.Parse(Guid.NewGuid().ToString("D").ToLowerInvariant()),
        Name = name,
        Components = { transform },
    };

    private static PrefabDocument MakeSource() => new()
    {
        Name = "Pillar",
        Guid = new AssetGuid(Guid.NewGuid()),
        Entities =
        {
            EntityDoc("Pillar", TransformDoc()),
            EntityDoc("Capital", TransformDoc(1, 2, 3)),
        },
    };

    private static (GameEntity root, GameEntity child) MakeInstance(float rootX = 0,
        float childZ = 3, float childSY = 1)
    {
        var root = new GameEntity("Pillar");
        root.Components.Add(new Transform { PositionX = rootX });
        var child = new GameEntity("Capital");
        child.Components.Add(new Transform { PositionX = 1, PositionY = 2, PositionZ = childZ, ScaleY = childSY });
        root.Children.Add(child);
        return (root, child);
    }

    private static List<GameEntity> Subtree(GameEntity root)
    {
        var all = new List<GameEntity> { root };
        Collect(root);
        return all;

        void Collect(GameEntity e)
        {
            foreach (var c in e.Children) { all.Add(c); Collect(c); }
        }
    }

    [Fact]
    public void Capture_SkipsRootPosition_ByDefault()
    {
        var source = MakeSource();
        var (root, child) = MakeInstance(rootX: 5, childZ: 9);
        var comp = new PrefabInstanceComponent();

        var (overrides, report) = PrefabOverrideService.Capture(root, Subtree(root), source);
        // 根 pos(5,0,0) 是布设意图：默认不捕获；子 posZ 3→9 捕获
        var prop = Assert.Single(overrides);
        Assert.Equal("Capital", prop.EntityName);
        Assert.Equal("posZ", prop.Property);
        Assert.Equal(9f, prop.Value);
        Assert.Empty(report.UnmappedInstanceNames);
        Assert.Empty(report.StaleOverrideNames);
    }

    [Fact]
    public void Capture_WithRootPosition_WhenExplicitlyRequested()
    {
        var source = MakeSource();
        var (root, child) = MakeInstance(rootX: 5, childZ: 3);   // 子未改
        var (overrides, _) = PrefabOverrideService.Capture(root, Subtree(root), source, includeRootPosition: true);
        Assert.Equal(5f, Assert.Single(overrides).Value);
        Assert.Equal("posX", overrides[0].Property);
    }

    [Fact]
    public void Capture_IdenticalValues_ProduceNoOverrides()
    {
        var source = MakeSource();
        var (root, child) = MakeInstance(rootX: 0, childZ: 3);   // 与源一致
        var (overrides, _) = PrefabOverrideService.Capture(root, Subtree(root), source);
        Assert.Empty(overrides);   // 还原即清除：一致 = 无覆盖
    }

    [Fact]
    public void StoreAndLoad_RoundTrip()
    {
        var source = MakeSource();
        var (root, _) = MakeInstance(childZ: 9, childSY: 4);
        var comp = new PrefabInstanceComponent();
        var (overrides, _) = PrefabOverrideService.Capture(root, Subtree(root), source);
        PrefabOverrideService.Store(comp, overrides);

        Assert.True(comp.HasOverrides);
        Assert.Contains("Capital", comp.Overrides.Keys);
        var loaded = PrefabOverrideService.Load(comp);
        Assert.Equal(overrides.Count, loaded.Count);
        Assert.Contains(loaded, o => o.Property == "posZ" && o.Value == 9f);
        Assert.Contains(loaded, o => o.Property == "sclY" && o.Value == 4f);
    }

    [Fact]
    public void Revert_RestoresSourceValues_AndClearsEntries()
    {
        var source = MakeSource();
        var (root, child) = MakeInstance(childZ: 9);
        var comp = new PrefabInstanceComponent();
        PrefabOverrideService.Store(comp,
            PrefabOverrideService.Capture(root, Subtree(root), source).overrides);

        var report = PrefabOverrideService.Revert(root, Subtree(root), source, comp);
        Assert.Equal(3f, child.GetTransform()!.PositionZ, 5);   // 回到源值
        Assert.False(comp.HasOverrides);                         // 条目清除
        Assert.Single(report.Applied);
    }

    [Fact]
    public void Apply_PromotesToSource_AndPersistsThroughSerializer()
    {
        var source = MakeSource();
        var (root, _) = MakeInstance(childZ: 9, childSY: 4);
        var comp = new PrefabInstanceComponent();
        PrefabOverrideService.Store(comp,
            PrefabOverrideService.Capture(root, Subtree(root), source).overrides);

        var (modified, report) = PrefabOverrideService.Apply(source, comp);
        Assert.Equal(new[] { "Capital" }, modified);
        Assert.False(comp.HasOverrides);
        Assert.Empty(report.StaleOverrideNames);

        // .prefab 落盘重载：提升值一致（保存重开语义）
        var path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"pf_{Guid.NewGuid():N}.prefab");
        PrefabSerializer.Write(path, source);
        var reloaded = PrefabSerializer.Read(path);
        Assert.NotNull(reloaded);
        var capital = reloaded!.Entities.Single(e => e.Name == "Capital");
        var t = JsonSerializer.Deserialize<Transform>(
            capital.Components.Single(c => c.Type == "Transform").Data.GetRawText(),
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        Assert.Equal(9f, t!.PositionZ, 5);
        Assert.Equal(4f, t.ScaleY, 5);
    }

    [Fact]
    public void Propagate_TakesSourceExceptOverridden_AndNeverTouchesRootPosition()
    {
        var source = MakeSource();
        var (root, child) = MakeInstance(rootX: 5, childZ: 9);
        var comp = new PrefabInstanceComponent();
        PrefabOverrideService.Store(comp,
            PrefabOverrideService.Capture(root, Subtree(root), source).overrides);

        // 源更新：Capital sclY 1→7（实例有 posZ 覆盖）
        var capitalDoc = source.Entities.Single(e => e.Name == "Capital");
        capitalDoc.Components[0] = TransformDoc(1, 2, 3, sy: 7);

        var report = PrefabOverrideService.Propagate(root, Subtree(root), source, comp);
        Assert.Equal(7f, child.GetTransform()!.ScaleY, 5);       // 无覆盖属性取源值
        Assert.Equal(9f, child.GetTransform()!.PositionZ, 5);    // 覆盖保留
        Assert.Equal(5f, root.GetTransform()!.PositionX, 5);     // 根布设不触碰
        Assert.Contains(report.Applied, a => a.EntityName == "Capital" && a.Property == "sclY");
    }

    [Fact]
    public void Diagnostics_StaleUnmappedAndDuplicateNames_AreReported()
    {
        var source = MakeSource();
        source.Entities.Add(EntityDoc("Capital", TransformDoc()));   // 源内重名

        var (root, _) = MakeInstance(childZ: 9);
        var extra = new GameEntity("Extra");
        extra.Components.Add(new Transform());
        root.Children.Add(extra);
        var subtree = Subtree(root);
        subtree.Add(new GameEntity("Ghost"));   // 无 transform——不参与

        var (_, report) = PrefabOverrideService.Capture(root, subtree, source);
        Assert.Contains("Extra", report.UnmappedInstanceNames);
        Assert.Contains("Capital", report.DuplicateSourceNames);

        // stale：覆盖指向已删源实体
        var comp = new PrefabInstanceComponent();
        PrefabOverrideService.Store(comp, new[]
        {
            new PrefabOverride { EntityName = "Ghost", Property = "posZ", Value = 1 },
        });
        var applyReport = PrefabOverrideService.Apply(source, comp).report;
        Assert.Contains("Ghost", applyReport.StaleOverrideNames);
    }

    [Fact]
    public void UndoAction_RestoresPreviousDictionary()
    {
        var comp = new PrefabInstanceComponent();
        PrefabOverrideService.Store(comp, new[]
        {
            new PrefabOverride { EntityName = "Capital", Property = "posZ", Value = 9 },
        });
        var action = new PrefabOverridesUndoAction(comp);
        PrefabOverrideService.Store(comp, new[]
        {
            new PrefabOverride { EntityName = "Capital", Property = "posZ", Value = 11 },
        });
        action.Commit();

        action.Undo();
        Assert.Equal(9f, PrefabOverrideService.Load(comp).Single().Value);
        action.Redo();
        Assert.Equal(11f, PrefabOverrideService.Load(comp).Single().Value);
    }
}
