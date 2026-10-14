using System;
using System.Linq;
using PrimalEditor_Avalonia.Terrain;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.TerrainNs;

/// <summary>
/// terrain-vegetation S1 契约测试：笔刷衰减（中心最强/圆外不触碰/确定性行主序）、
/// 五种操作语义、稀疏 Undo/Redo 精确还原、双线性采样、序列化往返（逐位）+
/// 未知版本拒、非法参数拒绝。
/// </summary>
public sealed class TerrainTests
{
    private static (HeightfieldTerrain t, TerrainBrush b) Flat(int size = 32, double radius = 6,
        BrushOp op = BrushOp.Raise, double strength = 0.5)
    {
        return (HeightfieldTerrain.Create(size, size, 1), new TerrainBrush
        {
            Op = op, Radius = radius, Strength = strength, FalloffPow = 1,
        });
    }

    [Fact]
    public void Brush_RaiseFalloff_CenterMax_EdgeUntouched()
    {
        var (t, b) = Flat();
        var changes = TerrainBrushOps.Apply(t, b, 16, 16);
        Assert.NotEmpty(changes);
        // 中心格变化 = 全强度；恰好圆外的格不在变更集
        Assert.Equal(0.5, t.HeightAt(16, 16), 10);
        var center = changes.Single(c => c.Row == 16 && c.Col == 16);
        Assert.Equal(0.5, center.After - center.Before, 10);
        Assert.Contains(changes, c => Math.Abs(c.After - c.Before) < 0.5);       // 边缘衰减 < 中心
        var farthest = changes.Max(c => Math.Sqrt((c.Row - 16.0) * (c.Row - 16) + (c.Col - 16.0) * (c.Col - 16)));
        Assert.True(farthest <= 6.0 + 1e-9);                                      // 无圆外点
        // 行主序确定性：同输入再跑（重置后）结果一致
        var (t2, b2) = Flat();
        var changes2 = TerrainBrushOps.Apply(t2, b2, 16, 16);
        Assert.Equal(
            changes.Select(c => (c.Row, c.Col, Math.Round(c.After, 12))).ToList(),
            changes2.Select(c => (c.Row, c.Col, Math.Round(c.After, 12))).ToList());
    }

    [Fact]
    public void Brush_LowerSmoothFlattenSet_Semantics()
    {
        // Lower：中心 -0.5
        var (t, b) = Flat(op: BrushOp.Lower);
        TerrainBrushOps.Apply(t, b, 16, 16);
        Assert.Equal(-0.5, t.HeightAt(16, 16), 10);

        // Smooth：孤峰被拉向邻域均值（变化方向确定、量级 < 1）
        var (ts, bs) = Flat(op: BrushOp.Smooth, strength: 1);
        for (int r = 13; r <= 19; ++r)
            for (int c = 13; c <= 19; ++c)
                ts.Heights[r * ts.Cols + c] = 10;
        ts.Heights[16 * ts.Cols + 16] = 20;    // 孤峰
        TerrainBrushOps.Apply(ts, bs, 16, 16);
        Assert.True(ts.HeightAt(16, 16) < 20); // 向均值靠拢
        Assert.True(ts.HeightAt(16, 16) > 10);

        // Flatten：中心混合向目标 5
        var (tf, _) = Flat(op: BrushOp.Flatten, strength: 1);
        TerrainBrushOps.Apply(tf, new TerrainBrush { Op = BrushOp.Flatten, Radius = 6, Strength = 1, TargetHeight = 5 }, 16, 16);
        Assert.Equal(5.0, tf.HeightAt(16, 16), 10);

        // Set：按衰减直接设目标（中心=目标）
        var (tx, bx) = Flat(op: BrushOp.Set);
        TerrainBrushOps.Apply(tx, new TerrainBrush { Op = BrushOp.Set, Radius = 6, Strength = 1, TargetHeight = 3 }, 16, 16);
        Assert.Equal(3.0, tx.HeightAt(16, 16), 10);
        Assert.Equal(0.0, tx.HeightAt(0, 0), 10);                              // 圆外原值
    }

    [Fact]
    public void Stroke_UndoRedo_RestoresExactly()
    {
        var (t, b) = Flat();
        TerrainBrushOps.Apply(t, new TerrainBrush { Op = BrushOp.Raise, Radius = 5, Strength = 0.3 }, 10, 20);
        var before = (double[])t.Heights.Clone();
        var changes = TerrainBrushOps.Apply(t, new TerrainBrush { Op = BrushOp.Smooth, Radius = 6, Strength = 0.8 }, 10, 20);
        var action = new TerrainBrushStrokeAction(t, changes);

        action.Undo();
        Assert.Equal(before, t.Heights);       // 逐位还原
        action.Redo();
        Assert.NotEqual(before, t.Heights);
        action.Undo();
        Assert.Equal(before, t.Heights);
    }

    [Fact]
    public void Sampling_Bilinear_BetweenGridPoints()
    {
        var t = HeightfieldTerrain.Create(2, 2, 1);
        t.Heights[0] = 0; t.Heights[1] = 1;     // row0: 0,1
        t.Heights[2] = 2; t.Heights[3] = 3;     // row1: 2,3
        Assert.Equal(0.0, t.HeightAt(0, 0), 12);        // 格点 (r0,c0)
        Assert.Equal(1.0, t.HeightAt(1, 0), 12);        // x=1,z=0 → 格点 (r0,c1)
        Assert.Equal(2.0, t.HeightAt(0, 1), 12);        // x=0,z=1 → 格点 (r1,c0)
        Assert.Equal(1.5, t.HeightAt(0.5, 0.5), 12);    // 四格均值
    }

    [Fact]
    public void Serialization_RoundTripBitExact_AndFutureRejected()
    {
        var (t, b) = Flat();
        TerrainBrushOps.Apply(t, b, 16, 16);
        var (loaded, err) = HeightfieldTerrain.Load(t.Serialize());
        Assert.Null(err);
        Assert.Equal(t.Heights, loaded!.Heights);   // R 格式字符串 → 逐位一致
        Assert.Equal(t.Rows, loaded.Rows);

        var (future, futureErr) = HeightfieldTerrain.Load(
            t.Serialize().Replace("\"schemaVersion\":1", "\"schemaVersion\":9"));
        Assert.Null(future);
        Assert.Contains("不静默覆写", futureErr);
    }

    [Fact]
    public void Validation_RejectsInvalidShapes()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => HeightfieldTerrain.Create(0, 10, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => HeightfieldTerrain.Create(10, 10, 0));
        var (t, _) = Flat();
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            TerrainBrushOps.Apply(t, new TerrainBrush { Radius = 0, Strength = 1 }, 0, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            TerrainBrushOps.Apply(t, new TerrainBrush { Radius = 1, Strength = 0 }, 0, 0));
    }
}
