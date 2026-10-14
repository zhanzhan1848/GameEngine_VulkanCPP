using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Content.Printing;

/// <summary>打印几何检查发现（3d-printing S1，2026-10-15）。阈值缺参数的项
/// 标 NotChecked（"未判定"）——ValidationErrors=0 ≠ 打印合格（阶段合同）。</summary>
public enum PrintCheckSeverity
{
    Info = 0,
    Warning = 1,
    Error = 2,
    /// <summary>缺少工艺/材料参数无法判定（薄壁等）——不绿不红。</summary>
    NotChecked = 3,
}

public sealed class PrintCheckFinding
{
    public PrintCheckSeverity Severity { get; init; }
    /// <summary>稳定检查项 ID（watertight/boundaryHoles/nonManifold/degenerate/
    /// flippedWinding/shells/thinWall/dimensions）。</summary>
    public string Check { get; init; } = "";
    public string Message { get; init; } = "";
    /// <summary>量化值（边界边数/壳数/面积…；无则 null）。</summary>
    public double? Value { get; init; }
}

public sealed class PrintMeshReport
{
    public List<PrintCheckFinding> Findings { get; } = new();
    /// <summary>包围盒尺寸（模型单位）。索引 [0]=x [1]=y [2]=z。</summary>
    public double[] Size { get; init; } = { 0, 0, 0 };
    public long TriangleCount { get; init; }
    public long VertexCount { get; init; }
    public bool HasErrors => Findings.Any(f => f.Severity == PrintCheckSeverity.Error);
}

/// <summary>打印单位（3MF 明确单位；STL 无单位语义——导出旁附清单声明）。</summary>
public enum PrintUnit { Millimeter, Centimeter, Meter, Inch }

public static class PrintUnitFactors
{
    /// <summary>单位 → 毫米。</summary>
    public static double ToMillimeters(PrintUnit unit) => unit switch
    {
        PrintUnit.Millimeter => 1.0,
        PrintUnit.Centimeter => 10.0,
        PrintUnit.Meter => 1000.0,
        PrintUnit.Inch => 25.4,
        _ => 1.0,
    };
}

/// <summary>
/// 制造几何检查（3d-printing S1）。纯函数：输入模型空间三角形汤（positions
/// xyz 三元组 + 三角索引），输出结构化报告。检查面：
/// 水密（边流形且无边界）、边界孔洞（含环数）、非流形边、退化面、绕向不一致、
/// 独立壳数、包围盒尺寸。薄壁需要工艺参数——无参数时 NotChecked。
/// </summary>
public static class PrintMeshChecks
{
    public static PrintMeshReport Run(
        IReadOnlyList<double> positions, IReadOnlyList<int> triangles,
        double? thinWallMillimeters = null)
    {
        var findings = new List<PrintCheckFinding>();
        if (triangles.Count % 3 != 0)
        {
            findings.Add(new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.Error, Check = "input",
                Message = $"三角索引数 {triangles.Count} 不是 3 的倍数",
            });
            var early = new PrintMeshReport { TriangleCount = triangles.Count / 3, VertexCount = positions.Count / 3 };
            foreach (var f in findings) early.Findings.Add(f);
            return early;
        }
        var triCount = triangles.Count / 3;

        // ---- 尺寸 ----
        double[] size = { 0, 0, 0 };
        if (positions.Count >= 3)
        {
            double mn0 = positions[0], mx0 = positions[0];
            double mn1 = positions[1], mx1 = positions[1];
            double mn2 = positions[2], mx2 = positions[2];
            for (int i = 0; i + 2 < positions.Count; i += 3)
            {
                mn0 = Math.Min(mn0, positions[i]); mx0 = Math.Max(mx0, positions[i]);
                mn1 = Math.Min(mn1, positions[i + 1]); mx1 = Math.Max(mx1, positions[i + 1]);
                mn2 = Math.Min(mn2, positions[i + 2]); mx2 = Math.Max(mx2, positions[i + 2]);
            }
            size[0] = mx0 - mn0; size[1] = mx1 - mn1; size[2] = mx2 - mn2;
        }
        findings.Add(new PrintCheckFinding
        {
            Severity = PrintCheckSeverity.Info, Check = "dimensions",
            Message = $"包围盒 {size[0]:F3} × {size[1]:F3} × {size[2]:F3}（模型单位）",
        });

        // ---- 退化面 ----
        long degenerate = 0;
        for (int t = 0; t < triangles.Count; t += 3)
        {
            var a = triangles[t] * 3; var b = triangles[t + 1] * 3; var c = triangles[t + 2] * 3;
            if (triangles[t] == triangles[t + 1] || triangles[t + 1] == triangles[t + 2] || triangles[t] == triangles[t + 2])
            { ++degenerate; continue; }
            var abx = positions[b] - positions[a]; var aby = positions[b + 1] - positions[a + 1]; var abz = positions[b + 2] - positions[a + 2];
            var acx = positions[c] - positions[a]; var acy = positions[c + 1] - positions[a + 1]; var acz = positions[c + 2] - positions[a + 2];
            var cx = aby * acz - abz * acy;
            var cy = abz * acx - abx * acz;
            var cz = abx * acy - aby * acx;
            if (cx * cx + cy * cy + cz * cz < 1e-24) ++degenerate;
        }
        if (degenerate > 0)
            findings.Add(new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.Error, Check = "degenerate",
                Message = $"{degenerate} 个退化面（零面积/重复索引）", Value = degenerate,
            });

        // ---- 边统计（有向 + 无向）：流形/边界/绕向 ----
        var directed = new Dictionary<(int, int), int>();
        var undirected = new Dictionary<(int, int), int>();
        for (int t = 0; t < triangles.Count; t += 3)
        {
            for (int e = 0; e < 3; ++e)
            {
                var u = triangles[t + e];
                var v = triangles[t + (e + 1) % 3];
                var key = (Math.Min(u, v), Math.Max(u, v));
                undirected[key] = undirected.GetValueOrDefault(key) + 1;
                directed[(u, v)] = directed.GetValueOrDefault((u, v)) + 1;
            }
        }
        long boundaryEdges = undirected.Values.LongCount(c => c == 1);
        long nonManifold = undirected.Values.LongCount(c => c > 2);
        long sameDirection = directed.Values.LongCount(c => c > 1);   // 同向重复 = 绕向不一致信号

        if (boundaryEdges > 0)
            findings.Add(new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.Error, Check = "boundaryHoles",
                Message = $"{boundaryEdges} 条边界边（网格未闭合）", Value = boundaryEdges,
            });
        if (nonManifold > 0)
            findings.Add(new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.Error, Check = "nonManifold",
                Message = $"{nonManifold} 条非流形边（被 >2 个面共享）", Value = nonManifold,
            });
        if (sameDirection > 0)
            findings.Add(new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.Warning, Check = "flippedWinding",
                Message = $"{sameDirection} 对同向边（存在绕向翻转的面）", Value = sameDirection,
            });
        findings.Add(new PrintCheckFinding
        {
            Severity = boundaryEdges == 0 && nonManifold == 0
                ? PrintCheckSeverity.Info : PrintCheckSeverity.Error,
            Check = "watertight",
            Message = boundaryEdges == 0 && nonManifold == 0
                ? "水密（边流形且无边界）"
                : "非水密——切片会产生非闭合层",
        });

        // ---- 朝向（仅水密时有意义）：有向体积 < 0 = 法线整体朝内 ----
        if (boundaryEdges == 0 && nonManifold == 0)
        {
            double volume6 = 0;
            for (int t = 0; t < triangles.Count; t += 3)
            {
                var a = triangles[t] * 3; var b = triangles[t + 1] * 3; var c = triangles[t + 2] * 3;
                volume6 += positions[a] * (positions[b + 1] * positions[c + 2] - positions[b + 2] * positions[c + 1])
                         - positions[b] * (positions[a + 1] * positions[c + 2] - positions[a + 2] * positions[c + 1])
                         + positions[c] * (positions[a + 1] * positions[b + 2] - positions[a + 2] * positions[b + 1]);
            }
            if (volume6 < -1e-12)
                findings.Add(new PrintCheckFinding
                {
                    Severity = PrintCheckSeverity.Warning, Check = "orientation",
                    Message = "法线整体朝内（有向体积为负）——切片前需翻转", Value = volume6 / 6.0,
                });
        }

        // ---- 壳（共享顶点并查集） ----
        int vertexCount = positions.Count / 3;
        var parent = Enumerable.Range(0, vertexCount).ToArray();
        int Find(int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; }
        for (int t = 0; t < triangles.Count; t += 3)
        {
            var ra = Find(triangles[t]); var rb = Find(triangles[t + 1]); var rc = Find(triangles[t + 2]);
            if (ra != rb) parent[ra] = rb;
            if (rb != rc) parent[rb] = rc;
        }
        var shells = new HashSet<int>();
        for (int t = 0; t < triangles.Count; t += 3) shells.Add(Find(triangles[t]));
        findings.Add(new PrintCheckFinding
        {
            Severity = shells.Count > 1 ? PrintCheckSeverity.Warning : PrintCheckSeverity.Info,
            Check = "shells",
            Message = shells.Count == 1 ? "单一闭合壳" : $"{shells.Count} 个独立壳（确认是否都要打印）",
            Value = shells.Count,
        });

        // ---- 薄壁：需要工艺参数 ----
        findings.Add(thinWallMillimeters is { } wall
            ? new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.NotChecked, Check = "thinWall",
                Message = $"薄壁按阈值 {wall:F2}mm 的采样检查是后续切片（S1 未实现几何距离场）",
            }
            : new PrintCheckFinding
            {
                Severity = PrintCheckSeverity.NotChecked, Check = "thinWall",
                Message = "未提供工艺/材料参数——薄壁未判定（不是合格）",
            });

        var report = new PrintMeshReport
        {
            Size = size,
            TriangleCount = triCount,
            VertexCount = vertexCount,
        };
        foreach (var f in findings) report.Findings.Add(f);
        return report;
    }
}
