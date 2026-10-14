using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Terrain;

/// <summary>笔刷操作类型（terrain-vegetation S1，2026-10-15）。</summary>
public enum BrushOp
{
    /// <summary>抬升（×衰减掩码×强度）。</summary>
    Raise = 0,
    /// <summary>下压。</summary>
    Lower = 1,
    /// <summary>平滑（3×3 邻域均值，×衰减）。</summary>
    Smooth = 2,
    /// <summary>推平到目标高度（按衰减混合）。</summary>
    Flatten = 3,
    /// <summary>直接置目标高度（按衰减混合）。</summary>
    Set = 4,
}

/// <summary>圆形笔刷参数（纯数据）。</summary>
public sealed class TerrainBrush
{
    public BrushOp Op { get; init; } = BrushOp.Raise;
    /// <summary>半径（米，>0）。</summary>
    public double Radius { get; init; } = 4;
    /// <summary>强度（每应用一次的最大高度变化，米；>0）。</summary>
    public double Strength { get; init; } = 0.5;
    /// <summary>Flatten/Set 的目标高度（米）。</summary>
    public double TargetHeight { get; init; }
    /// <summary>衰减幂（1=线性；0=平顶）。</summary>
    public double FalloffPow { get; init; } = 1;
}

/// <summary>
/// 高度场地形（terrain-vegetation S1）。
///
/// 路线决策（子规划 S1 进入条件）：**高度场**——S1/S2 的编辑/保存/Undo 均以
/// 高度场为唯一数据模型；体素（悬挑/洞）不混入本模型，确需时按子规划独立
/// 立项。渲染走现有网格管线（自生成三角网格），地形渲染优化延后随渲染专项。
/// 行主序存储；HeightAt 双线性采样；越界读 clamp、写拒绝（类型化）。
/// </summary>
public sealed class HeightfieldTerrain
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public int Rows { get; init; }
    public int Cols { get; init; }
    /// <summary>单元尺寸（米，>0）。原点在 (0,0)，+X 列、+Z 行。</summary>
    public double CellSize { get; init; } = 1;

    /// <summary>行主序高度（米）。长度恒为 Rows×Cols。</summary>
    public double[] Heights { get; init; } = Array.Empty<double>();

    public static HeightfieldTerrain Create(int rows, int cols, double cellSize, double fill = 0)
    {
        if (rows <= 0 || cols <= 0) throw new ArgumentOutOfRangeException("行列数必须 > 0");
        if (cellSize <= 0 || !double.IsFinite(cellSize)) throw new ArgumentOutOfRangeException(nameof(cellSize));
        var t = new HeightfieldTerrain { Rows = rows, Cols = cols, CellSize = cellSize, Heights = new double[rows * cols] };
        Array.Fill(t.Heights, fill);
        return t;
    }

    public double Get(int row, int col)
    {
        row = Math.Clamp(row, 0, Rows - 1);
        col = Math.Clamp(col, 0, Cols - 1);
        return Heights[row * Cols + col];
    }

    /// <summary>世界坐标（x,z）双线性采样。</summary>
    public double HeightAt(double x, double z)
    {
        var gx = x / CellSize;
        var gz = z / CellSize;
        var c0 = (int)Math.Floor(gx);
        var r0 = (int)Math.Floor(gz);
        var fx = gx - c0;
        var fz = gz - r0;
        return (Get(r0, c0) * (1 - fx) + Get(r0, c0 + 1) * fx) * (1 - fz)
             + (Get(r0 + 1, c0) * (1 - fx) + Get(r0 + 1, c0 + 1) * fx) * fz;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = false,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(new DTO
    {
        SchemaVersion = SchemaVersion,
        Rows = Rows,
        Cols = Cols,
        CellSize = CellSize,
        Heights = Heights.Select(h => h.ToString("R")).ToArray(),
    }, JsonOptions);

    public static (HeightfieldTerrain? terrain, string? error) Load(string json)
    {
        try
        {
            var dto = JsonSerializer.Deserialize<DTO>(json, JsonOptions);
            if (dto == null) return (null, "地形反序列化为 null");
            if (dto.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {dto.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写");
            if (dto.Rows <= 0 || dto.Cols <= 0 || dto.Heights.Length != dto.Rows * dto.Cols)
                return (null, "地形尺寸与高度数组不一致");
            var t = new HeightfieldTerrain
            {
                Rows = dto.Rows, Cols = dto.Cols, CellSize = dto.CellSize,
                Heights = dto.Heights.Select(h => double.Parse(h, System.Globalization.CultureInfo.InvariantCulture)).ToArray(),
            };
            return (t, null);
        }
        catch (Exception ex) when (ex is JsonException or FormatException)
        {
            return (null, $"地形文档非法: {ex.Message}");
        }
    }

    private sealed class DTO
    {
        public int SchemaVersion { get; set; }
        public int Rows { get; set; }
        public int Cols { get; set; }
        public double CellSize { get; set; }
        public string[] Heights { get; set; } = Array.Empty<string>();
    }
}

/// <summary>单点变更（稀疏 Undo 的数据单元）。</summary>
public readonly record struct HeightChange(int Row, int Col, double Before, double After);

/// <summary>
/// 笔刷应用（纯函数）：返回受影响点的变更列表（行主序确定性）。圆外点不触碰；
/// 衰减掩码 = (1 - d/R)^falloffPow（d>R → 0）。
/// </summary>
public static class TerrainBrushOps
{
    public static List<HeightChange> Apply(HeightfieldTerrain t, TerrainBrush b, double centerX, double centerZ)
    {
        if (b.Radius <= 0 || b.Strength <= 0)
            throw new ArgumentOutOfRangeException("笔刷半径/强度必须 > 0");
        var changes = new List<HeightChange>();
        int r0 = Math.Max(0, (int)Math.Floor((centerZ - b.Radius) / t.CellSize));
        int r1 = Math.Min(t.Rows - 1, (int)Math.Ceiling((centerZ + b.Radius) / t.CellSize));
        int c0 = Math.Max(0, (int)Math.Floor((centerX - b.Radius) / t.CellSize));
        int c1 = Math.Min(t.Cols - 1, (int)Math.Ceiling((centerX + b.Radius) / t.CellSize));
        for (int row = r0; row <= r1; ++row)
        {
            for (int col = c0; col <= c1; ++col)
            {
                var x = col * t.CellSize;
                var z = row * t.CellSize;
                var d = Math.Sqrt((x - centerX) * (x - centerX) + (z - centerZ) * (z - centerZ));
                if (d > b.Radius) continue;
                var mask = Math.Pow(1 - d / b.Radius, Math.Max(0, b.FalloffPow));
                var before = t.Heights[row * t.Cols + col];
                double after = b.Op switch
                {
                    BrushOp.Raise => before + b.Strength * mask,
                    BrushOp.Lower => before - b.Strength * mask,
                    BrushOp.Smooth => SmoothAt(t, row, col, mask),
                    BrushOp.Flatten => before + (b.TargetHeight - before) * mask * Math.Min(1, b.Strength),
                    BrushOp.Set => before + (b.TargetHeight - before) * mask,
                    _ => before,
                };
                if (after == before) continue;
                t.Heights[row * t.Cols + col] = after;
                changes.Add(new HeightChange(row, col, before, after));
            }
        }
        return changes;
    }

    private static double SmoothAt(HeightfieldTerrain t, int row, int col, double mask)
    {
        double sum = 0;
        int n = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
            {
                sum += t.Get(row + dr, col + dc);   // 越界 clamp 参与均值
                ++n;
            }
        var avg = sum / n;
        var cur = t.Heights[row * t.Cols + col];
        return cur + (avg - cur) * mask;
    }
}

/// <summary>笔刷一笔的 Undo/Redo（稀疏：只存受影响点的 before/after）。</summary>
public sealed class TerrainBrushStrokeAction : IUndoRedoAction
{
    private readonly HeightfieldTerrain _terrain;
    private readonly List<HeightChange> _changes;

    public string Name => $"Terrain {(_changes.Count > 0 ? "Brush" : "Noop")} ({_changes.Count} pts)";

    public TerrainBrushStrokeAction(HeightfieldTerrain terrain, List<HeightChange> changes)
    {
        _terrain = terrain;
        _changes = changes;
    }

    public void Undo()
    {
        for (int i = _changes.Count - 1; i >= 0; --i)   // 逆序还原（与 apply 相反方向）
        {
            var c = _changes[i];
            _terrain.Heights[c.Row * _terrain.Cols + c.Col] = c.Before;
        }
    }

    public void Redo()
    {
        foreach (var c in _changes)
            _terrain.Heights[c.Row * _terrain.Cols + c.Col] = c.After;
    }
}
