using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.WorldPartition;

/// <summary>分区单元键（large-world S1，2026-10-15）。简单网格；Ordinal 稳定。</summary>
public readonly record struct CellKey(int X, int Z);

/// <summary>分区中的实体记录。</summary>
public sealed class PartitionEntity
{
    public Guid EntityGuid { get; init; }
    public double X { get; init; }
    public double Z { get; init; }
}

/// <summary>
/// 世界分区网格（large-world S1）：简单网格验收（子规划 S1 语义）。纯逻辑——
/// 不接触引擎/渲染；HLOD 与大坐标渲染是渲染专项待协调项，本层只做分区调度、
/// 实体生命周期与状态保留的数据语义。
/// </summary>
public static class WorldPartitionGrid
{
    /// <summary>坐标 → 单元键（floor；负坐标向 −∞ 取整——数学一致无跳变）。</summary>
    public static CellKey CellOf(double x, double z, double cellSizeMeters) =>
        new((int)Math.Floor(x / cellSizeMeters), (int)Math.Floor(z / cellSizeMeters));

    /// <summary>单元中心（世界坐标）。</summary>
    public static (double x, double z) CellCenter(CellKey c, double cellSizeMeters) =>
        ((c.X + 0.5) * cellSizeMeters, (c.Z + 0.5) * cellSizeMeters);
}

/// <summary>
/// 分区场景：实体→单元分配、按半径的激活单元集、帧间差分（进入/退出）与
/// 实体生命周期事件流（确定性排序）、跨卸载的状态保留。
/// </summary>
public sealed class PartitionedScene
{
    private readonly double _cellSize;
    private readonly Dictionary<CellKey, List<PartitionEntity>> _cells = new();
    /// <summary>实体持久状态（卸载不清除——"状态保留"合同）。</summary>
    private readonly Dictionary<Guid, Dictionary<string, string>> _persistentState = new();

    public double CellSizeMeters => _cellSize;

    public PartitionedScene(double cellSizeMeters)
    {
        if (cellSizeMeters <= 0 || !double.IsFinite(cellSizeMeters))
            throw new ArgumentOutOfRangeException(nameof(cellSizeMeters), "单元尺寸必须为正有限值");
        _cellSize = cellSizeMeters;
    }

    public void Add(PartitionEntity e) =>
        Cell(e).Add(e);

    private List<PartitionEntity> Cell(PartitionEntity e)
    {
        var key = WorldPartitionGrid.CellOf(e.X, e.Z, _cellSize);
        if (!_cells.TryGetValue(key, out var list))
        {
            list = new List<PartitionEntity>();
            _cells[key] = list;
        }
        return list;
    }

    /// <summary>单元集合（升序枚举——确定性）。</summary>
    public IEnumerable<CellKey> OccupiedCells =>
        _cells.Keys.OrderBy(k => k.X).ThenBy(k => k.Z);

    public IReadOnlyList<PartitionEntity> EntitiesIn(CellKey cell) =>
        _cells.TryGetValue(cell, out var list) ? list : Array.Empty<PartitionEntity>();

    /// <summary>以视点为中心、半径覆盖的占用单元集合（半径换算为单元数，向上取整）。</summary>
    public IReadOnlyList<CellKey> CellsAround(double viewX, double viewZ, double radiusMeters)
    {
        if (radiusMeters < 0) throw new ArgumentOutOfRangeException(nameof(radiusMeters));
        var center = WorldPartitionGrid.CellOf(viewX, viewZ, _cellSize);
        var r = (int)Math.Ceiling(radiusMeters / _cellSize);
        return _cells.Keys
            .Where(k => Math.Abs(k.X - center.X) <= r && Math.Abs(k.Z - center.Z) <= r)
            .OrderBy(k => k.X).ThenBy(k => k.Z)
            .ToList();
    }

    /// <summary>帧间差分：old → new 的进入/退出单元（升序确定性）。</summary>
    public static (List<CellKey> entered, List<CellKey> exited) Diff(
        IReadOnlyCollection<CellKey> oldCells, IReadOnlyCollection<CellKey> newCells)
    {
        var entered = newCells.Except(oldCells).OrderBy(k => k.X).ThenBy(k => k.Z).ToList();
        var exited = oldCells.Except(newCells).OrderBy(k => k.X).ThenBy(k => k.Z).ToList();
        return (entered, exited);
    }

    /// <summary>单元生命周期事件流（进入=加载/退出=卸载；升序确定性）。
    /// 实体粒度事件由调用方按 EntitiesIn(cell) 展开——保持本层单一职责。</summary>
    public static List<(CellKey cell, bool enter)> LifecycleEvents(
        IReadOnlyCollection<CellKey> oldCells, IReadOnlyCollection<CellKey> newCells)
    {
        var (entered, exited) = Diff(oldCells, newCells);
        var events = new List<(CellKey, bool)>();
        events.AddRange(entered.Select(k => (k, true)));
        events.AddRange(exited.Select(k => (k, false)));
        return events.OrderBy(e => e.Item1.X).ThenBy(e => e.Item1.Z).ToList();
    }

    // ---- 状态保留 ----

    /// <summary>写实体持久状态（单元卸载后保留；重加载读回）。</summary>
    public void SaveState(Guid entity, string key, string value)
    {
        if (!_persistentState.TryGetValue(entity, out var bag))
        {
            bag = new Dictionary<string, string>(StringComparer.Ordinal);
            _persistentState[entity] = bag;
        }
        bag[key] = value;
    }

    /// <summary>读实体持久状态；无 → null。</summary>
    public string? LoadState(Guid entity, string key) =>
        _persistentState.TryGetValue(entity, out var bag)
        && bag.TryGetValue(key, out var v) ? v : null;

    /// <summary>模拟单元卸载：清空单元内实体登记，状态袋保留。</summary>
    public void UnloadCell(CellKey cell)
    {
        foreach (var e in EntitiesIn(cell))
        {
            // 状态保留在 _persistentState——卸载只清登记
        }
        _cells.Remove(cell);
    }
}
