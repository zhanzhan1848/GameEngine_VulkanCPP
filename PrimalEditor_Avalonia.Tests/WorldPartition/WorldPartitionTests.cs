using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.WorldPartition;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.WorldPartitionNs;

/// <summary>
/// large-world S1 契约测试：网格数学（floor 语义/负坐标/单元中心）、实体分配
/// 与单元查询、半径激活集、帧间差分与生命周期事件（确定性排序）、状态保留
/// 跨卸载、非法单元尺寸拒绝。
/// </summary>
public sealed class WorldPartitionTests
{
    [Fact]
    public void Grid_FloorSemantics_NegativeCoordinatesAndCenter()
    {
        Assert.Equal(new CellKey(0, 0), WorldPartitionGrid.CellOf(0, 0, 100));
        Assert.Equal(new CellKey(0, 0), WorldPartitionGrid.CellOf(99.9, 99.9, 100));
        Assert.Equal(new CellKey(1, 1), WorldPartitionGrid.CellOf(100, 100, 100));
        Assert.Equal(new CellKey(-1, -1), WorldPartitionGrid.CellOf(-0.1, -100, 100));  // 负向 −∞
        Assert.Equal(new CellKey(-1, 0), WorldPartitionGrid.CellOf(-100, 0, 100));   // 左闭右开
        Assert.Equal(new CellKey(-2, 0), WorldPartitionGrid.CellOf(-100.1, 0, 100));
        var (cx, cz) = WorldPartitionGrid.CellCenter(new CellKey(-1, 2), 100);
        Assert.Equal(-50.0, cx, 6);
        Assert.Equal(250.0, cz, 6);
    }

    [Fact]
    public void Scene_AssignmentAndCellQueries()
    {
        var scene = new PartitionedScene(100);
        var a = Guid.NewGuid();
        var b = Guid.NewGuid();
        scene.Add(new PartitionEntity { EntityGuid = a, X = 10, Z = 10 });
        scene.Add(new PartitionEntity { EntityGuid = b, X = -10, Z = 10 });   // 负单元

        var cells = scene.OccupiedCells.ToList();
        Assert.Equal(new CellKey(-1, 0), cells[0]);   // 升序确定性
        Assert.Equal(new CellKey(0, 0), cells[1]);
        Assert.Equal(b, scene.EntitiesIn(new CellKey(-1, 0)).Single().EntityGuid);
        Assert.Equal(a, scene.EntitiesIn(new CellKey(0, 0)).Single().EntityGuid);
        Assert.Empty(scene.EntitiesIn(new CellKey(9, 9)));
    }

    [Fact]
    public void Scene_RadiusActivation_AndDiffLifecycle()
    {
        var scene = new PartitionedScene(100);
        for (int x = -2; x <= 2; ++x)
            scene.Add(new PartitionEntity { EntityGuid = Guid.NewGuid(), X = x * 100 + 50, Z = 50 });

        // 视点 (150,50) 半径 120：中心 cell=1，r=ceil(1.2)=2 → x∈{-1..3}；
        // 占用 {-2..2} 的交集 = {-1,0,1,2}（cell 3 无实体、-2 在界外）
        var active = scene.CellsAround(150, 50, 120);
        Assert.Contains(new CellKey(0, 0), active);
        Assert.Contains(new CellKey(2, 0), active);
        Assert.DoesNotContain(new CellKey(-2, 0), active);
        Assert.DoesNotContain(new CellKey(3, 0), active);

        var (entered, exited) = PartitionedScene.Diff(
            oldCells: new[] { new CellKey(-2, 0), new CellKey(-1, 0) },
            newCells: new[] { new CellKey(0, 0), new CellKey(1, 0) });
        Assert.Equal(new[] { new CellKey(0, 0), new CellKey(1, 0) }, entered);
        Assert.Equal(new[] { new CellKey(-2, 0), new CellKey(-1, 0) }, exited);

        var events = PartitionedScene.LifecycleEvents(
            new[] { new CellKey(-1, 0), new CellKey(0, 0) },
            new[] { new CellKey(0, 0), new CellKey(1, 0) });
        // (0,0) 两侧都在 → 无事件；升序：(-1,0 退出) (1,0 进入)
        Assert.Equal((new CellKey(-1, 0), false), events[0]);
        Assert.Equal((new CellKey(1, 0), true), events[1]);
    }

    [Fact]
    public void State_RetainedAcrossCellUnload()
    {
        var scene = new PartitionedScene(100);
        var e = Guid.NewGuid();
        scene.Add(new PartitionEntity { EntityGuid = e, X = 50, Z = 50 });
        scene.SaveState(e, "health", "87");
        scene.SaveState(e, "opened", "true");

        scene.UnloadCell(new CellKey(0, 0));
        Assert.Empty(scene.EntitiesIn(new CellKey(0, 0)));        // 登记已清

        scene.Add(new PartitionEntity { EntityGuid = e, X = 50, Z = 50 });   // 重加载
        Assert.Equal("87", scene.LoadState(e, "health"));         // 状态保留
        Assert.Equal("true", scene.LoadState(e, "opened"));
        Assert.Null(scene.LoadState(Guid.NewGuid(), "health"));   // 无状态 → null
    }

    [Fact]
    public void InvalidCellSize_Rejected()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new PartitionedScene(0));
        Assert.Throws<ArgumentOutOfRangeException>(() => new PartitionedScene(-100));
        Assert.Throws<ArgumentOutOfRangeException>(() => new PartitionedScene(double.NaN));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            new PartitionedScene(100).CellsAround(0, 0, -1));     // 负半径同样拒绝
    }
}
