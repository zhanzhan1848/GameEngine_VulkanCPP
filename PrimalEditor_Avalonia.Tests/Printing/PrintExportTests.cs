using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;
using PrimalEditor_Avalonia.Content.Printing;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.PrintingNs;

/// <summary>
/// 3d-printing S1 契约测试：几何检查（水密/孔洞/非流形/退化/绕向/壳/尺寸/
/// 薄壁未判定）、单位换算、负缩放翻绕向、STL 二进制往返（导出重导入验证）、
/// 3MF 包结构（zip + unit + 三角计数）、STL 旁附清单。
/// 夹具：立方体（各面外法线绕向）、缺面立方体、双壳、退化/翻面三角。
/// </summary>
public sealed class PrintExportTests
{
    /// <summary>单位立方体三角形汤（12 面，外法线一致绕向）。中心在原点，边长 1。</summary>
    private static (List<double> positions, List<int> triangles) Cube(
        double cx = 0, double cy = 0, double cz = 0, double size = 1)
    {
        var s = size / 2;
        var p = new List<double>();
        int V(double x, double y, double z)
        {
            p.Add(cx + x * s); p.Add(cy + y * s); p.Add(cz + z * s);
            return p.Count / 3 - 1;
        }
        var tris = new List<int>();
        void Quad(int a, int b, int c, int d)
        {
            // 外法线绕向：读者从体外看 a-d-c-b 逆时针
            tris.AddRange(new[] { a, d, c, a, c, b });
        }
        var v000 = V(-1, -1, -1); var v100 = V(1, -1, -1); var v110 = V(1, 1, -1); var v010 = V(-1, 1, -1);
        var v001 = V(-1, -1, 1); var v101 = V(1, -1, 1); var v111 = V(1, 1, 1); var v011 = V(-1, 1, 1);
        Quad(v000, v100, v110, v010);   // -Z
        Quad(v101, v001, v011, v111);   // +Z
        Quad(v001, v000, v010, v011);   // -X
        Quad(v100, v101, v111, v110);   // +X
        Quad(v010, v110, v111, v011);   // +Y
        Quad(v000, v001, v101, v100);   // -Y
        return (p, tris);
    }

    private static PrintCheckFinding? Find(PrintMeshReport r, string check) =>
        r.Findings.FirstOrDefault(f => f.Check == check);

    [Fact]
    public void Checks_ClosedCube_IsWatertightSingleShell()
    {
        var (p, t) = Cube(size: 2);
        var report = PrintMeshChecks.Run(p, t);
        Assert.False(report.HasErrors);
        Assert.Equal(12, report.TriangleCount);
        Assert.Equal(PrintCheckSeverity.Info, Find(report, "watertight")!.Severity);
        Assert.Equal(1, Find(report, "shells")!.Value);
        Assert.Null(Find(report, "boundaryHoles"));
        Assert.Null(Find(report, "degenerate"));
        Assert.Null(Find(report, "flippedWinding"));
        Assert.Equal(2.0, report.Size[0], 6);
        Assert.Equal(2.0, report.Size[1], 6);
        Assert.Equal(2.0, report.Size[2], 6);
    }

    [Fact]
    public void Checks_MissingFace_ReportsBoundaryHolesAndNotWatertight()
    {
        var (p, t) = Cube();
        t.RemoveRange(0, 6);   // 去掉 -Z 面
        var report = PrintMeshChecks.Run(p, t);
        Assert.True(report.HasErrors);
        var holes = Find(report, "boundaryHoles")!;
        Assert.Equal(PrintCheckSeverity.Error, holes.Severity);
        Assert.Equal(4.0, holes.Value!.Value);              // 4 条边界边
        Assert.Equal(PrintCheckSeverity.Error, Find(report, "watertight")!.Severity);
    }

    [Fact]
    public void Checks_DegenerateAndFlippedTriangles_AreFlagged()
    {
        var (p, t) = Cube();
        // 退化面：重复索引
        t.AddRange(new[] { 0, 0, 1 });
        // 翻面：把一个已有面反向重画
        t.AddRange(new[] { 0, 2, 1 });
        var report = PrintMeshChecks.Run(p, t);
        Assert.Equal(1.0, Find(report, "degenerate")!.Value!.Value);
        var flipped = Find(report, "flippedWinding");
        Assert.Equal(PrintCheckSeverity.Warning, flipped!.Severity);
        Assert.True(flipped.Value!.Value >= 3);             // 3 条同向边
    }

    [Fact]
    public void Checks_TwoSeparatedCubes_ReportTwoShells()
    {
        var (p1, t1) = Cube();
        var (p2, t2) = Cube(cx: 10);
        var p = p1.Concat(p2).ToList();
        var t = t1.Concat(t2.Select(i => i + p1.Count / 3)).ToList();
        var report = PrintMeshChecks.Run(p, t);
        Assert.Equal(2.0, Find(report, "shells")!.Value);
        Assert.Equal(11.0, report.Size[0], 6);              // 10 间隔 + 1 边长
    }

    [Fact]
    public void Checks_ThinWall_IsNotCheckedWithoutParams()
    {
        var (p, t) = Cube();
        var report = PrintMeshChecks.Run(p, t);
        var thin = Find(report, "thinWall")!;
        Assert.Equal(PrintCheckSeverity.NotChecked, thin.Severity);
        Assert.Contains("未判定", thin.Message);             // ≠ 合格
    }

    [Fact]
    public void Units_Factors_AreExact()
    {
        Assert.Equal(1.0, PrintUnitFactors.ToMillimeters(PrintUnit.Millimeter));
        Assert.Equal(10.0, PrintUnitFactors.ToMillimeters(PrintUnit.Centimeter));
        Assert.Equal(1000.0, PrintUnitFactors.ToMillimeters(PrintUnit.Meter));
        Assert.Equal(25.4, PrintUnitFactors.ToMillimeters(PrintUnit.Inch));
    }

    [Fact]
    public void Transform_NegativeScale_FlipsWinding_AndUnitConverts()
    {
        var (p, t) = Cube(size: 10);   // 米为模型单位
        var (tp, tt) = MeshExporters.TransformToUnit(p, t,
            new double[] { 0, 0, 0 }, new[] { -1.0, 1.0, 1.0 },
            PrintUnit.Meter, PrintUnit.Millimeter);
        // 负 X 缩放：绕向翻转后第一个三角应为 (0,2,1)
        Assert.Equal(0, tt[0]);
        Assert.Equal(2, tt[1]);     // 外朝向首三角 (0,3,2) → 负缩放翻成 (0,2,3)
        Assert.Equal(3, tt[2]);
        // 米 → 毫米 ×1000，负缩放镜像后 X 范围仍 ±5×1000
        Assert.Equal(5000.0, Math.Abs(tp[0]), 4);
    }

    [Fact]
    public void Stl_RoundTrip_TriangleCountAndVerticesSurvive()
    {
        var (p, t) = Cube(size: 25);   // 已是毫米语义
        var path = Path.Combine(Path.GetTempPath(), $"stl_{Guid.NewGuid():N}.stl");
        MeshExporters.WriteStl(path, p, t, "unit-test cube");
        try
        {
            using var fs = File.OpenRead(path);
            using var r = new BinaryReader(fs);
            fs.Seek(80, SeekOrigin.Begin);
            Assert.Equal(12u, r.ReadUInt32());
            float nxFirst = 0, nzFirst = 0;
            for (int i = 0; i < 12; ++i)
            {
                var n = new[] { r.ReadSingle(), r.ReadSingle(), r.ReadSingle() };
                for (int v = 0; v < 9; ++v) r.ReadSingle();
                r.ReadUInt16();
                if (i == 0) { nxFirst = n[0]; nzFirst = n[2]; }
            }
            Assert.Equal(0.0, nxFirst, 4);    // 首面是 -Z：法线 (0,0,-1)（外法线）
            Assert.Equal(-1.0, nzFirst, 4);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void ThreeMf_ContainsUnitTriangleCountsAndZipStructure()
    {
        var (p, t) = Cube(size: 30);
        var path = Path.Combine(Path.GetTempPath(), $"mf_{Guid.NewGuid():N}.3mf");
        MeshExporters.Write3Mf(path, p, t, PrintUnit.Millimeter, "unit-cube");
        try
        {
            using var zip = ZipFile.OpenRead(path);
            Assert.Contains(zip.Entries, e => e.FullName == "[Content_Types].xml");
            Assert.Contains(zip.Entries, e => e.FullName == "_rels/.rels");
            var model = zip.GetEntry("3D/3dmodel.model");
            Assert.NotNull(model);
            using var sr = new StreamReader(model!.Open());
            var xml = sr.ReadToEnd();
            Assert.Contains("unit=\"millimeter\"", xml);
            Assert.Equal(8, Regex.Matches(xml, "<vertex ").Count);
            Assert.Equal(12, Regex.Matches(xml, "<triangle ").Count);
            Assert.Contains("<item objectid=\"1\"/>", xml);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void StlSidecar_DeclaresUnitAndSize()
    {
        var (p, t) = Cube(size: 42);
        var path = Path.Combine(Path.GetTempPath(), $"st_{Guid.NewGuid():N}.stl");
        MeshExporters.WriteStlSidecar(path, PrintUnit.Millimeter, new[] { 42.0, 42.0, 42.0 }, "test-source");
        var sidecar = Path.ChangeExtension(path, ".stl.txt");
        try
        {
            var text = File.ReadAllText(sidecar);
            Assert.Contains("unit: Millimeter", text);
            Assert.Contains("42", text);
            Assert.Contains("no unit semantics", text);
        }
        finally { File.Delete(sidecar); File.Delete(path); }
    }

    [Fact]
    public void Checks_InwardFacingWatertightMesh_WarnsOrientation()
    {
        var (p, t) = Cube();
        var flipped = new List<int>();
        for (int i = 0; i < t.Count; i += 3)
            flipped.AddRange(new[] { t[i], t[i + 2], t[i + 1] });   // 整体翻绕向 → 法线朝内
        var report = PrintMeshChecks.Run(p, flipped);
        Assert.False(report.HasErrors);                              // 仍水密（几何闭合）
        var orient = Find(report, "orientation");
        Assert.NotNull(orient);
        Assert.Equal(PrintCheckSeverity.Warning, orient!.Severity);  // 但朝内要警告
        Assert.True(orient.Value!.Value < 0);
    }
}
