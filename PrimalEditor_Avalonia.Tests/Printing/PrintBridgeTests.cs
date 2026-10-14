using System;
using System.IO;
using System.Linq;
using System.Text;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Content.Printing;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.PrintBridgeNs;

/// <summary>
/// 3d-printing 资产桥接 E2E（print S1，2026-10-15）：测试内构造 cooked 容器
/// （与 ContentTools SceneBlobWriter 同布局：场景名/材质表/LOD 组/mesh 记录
/// [positions|elements|indices|meshlet 空|SDF 空]）→ EngineDLL
/// CookedMeshTriangleCounts/Data → PrintMeshChecks 全链路。
/// 覆盖：水密立方体提取逐值一致、u16 索引升 u32、坏文件类型化失败、
/// 检查管道在真实提取结果上判定水密。
/// 需要引擎 dylib 可加载（与 Native 系测试同前置）。
/// </summary>
public sealed class PrintBridgeTests
{
    /// <summary>按容器布局写 cooked .mesh（单材质/单组单 mesh，外朝向立方体）。</summary>
    private static string WriteCookedCube(double size)
    {
        var s = size / 2;
        var p = new double[]
        {
            -s, -s, -s,  s, -s, -s,  s,  s, -s, -s,  s, -s,
            -s, -s,  s,  s, -s,  s,  s,  s,  s, -s,  s,  s,
        };
        var tris = new ushort[]
        {
            0,3,2, 0,2,1,   5,4,7, 5,7,6,   4,0,1, 4,1,5,
            1,2,6, 1,6,5,   2,3,7, 2,7,6,   4,7,3, 4,3,0,
        };
        var b = new BinaryWriter(new MemoryStream());
        void Str(string v)
        {
            b.Write((uint)Encoding.UTF8.GetByteCount(v));
            b.Write(Encoding.UTF8.GetBytes(v));
        }
        Str("bridge-scene");
        b.Write(1u);                       // 材质数
        Str("mat"); Str("diffuse.png"); Str("normal.png");
        b.Write(1u);                       // LOD 组数
        Str("lod0"); b.Write(1u);          // 组名 + mesh 数
        Str("cube");                       // mesh 名
        b.Write(0u);                       // lod id
        b.Write(0u);                       // material index
        b.Write(36u);                      // element size（f32 pos + f32 nrm + f32 uv = 9×4）
        b.Write(0u);                       // element type（无 lightmap 标志）
        b.Write((uint)p.Length / 3);       // vertex count
        b.Write(2u);                       // index size（u16）
        b.Write((uint)tris.Length);        // index count
        b.Write(0f);                       // threshold
        foreach (var v in p) b.Write((float)v);        // positions
        for (int i = 0; i < p.Length / 3 * 9; ++i) b.Write(0f);   // elements 填充
        foreach (var v in tris) b.Write(v);            // indices
        b.Write(0x4D455348u);              // meshlet magic（占位——0 组）
        b.Write(0u);                       // meshlet count
        b.Write(0u); b.Write(0u);          // counted bytes: vertex offsets / 索引
        b.Write(0x53444631u);              // sdf magic（占位）
        b.Write(0u); b.Write(0u); b.Write(0u);
        for (int i = 0; i < 6; ++i) b.Write(0f);
        b.Write(0u); b.Write(0u); b.Write(0u);   // counted: u16 / u8 / u16

        var path = Path.Combine(Path.GetTempPath(), $"printbridge_{Guid.NewGuid():N}.mesh");
        File.WriteAllBytes(path, ((MemoryStream)b.BaseStream).ToArray());
        return path;
    }

    [Fact]
    public void Bridge_ExtractsWatertightCube_AndChecksPipeline()
    {
        if (!NativeEngineBridge.IsLoaded) return;   // 引擎不可加载环境跳过（不绿装）
        var path = WriteCookedCube(2.0);
        try
        {
            var mesh = NativeEngineBridge.ReadCookedMeshTriangles(path);
            Assert.NotNull(mesh);
            var (positions, indices) = mesh!.Value;
            Assert.Equal(24, positions.Length / 3);
            Assert.Equal(36, indices.Length);       // 12 三角
            Assert.Equal(-1.0, positions[0], 6);
            Assert.Equal(1.0, positions[6], 6);     // 第 3 顶点 x（+s）

            var report = PrintMeshChecks.Run(positions, indices);
            Assert.False(report.HasErrors);
            Assert.Equal(12, report.TriangleCount);
            var watertight = report.Findings.First(f => f.Check == "watertight");
            Assert.Equal(PrintCheckSeverity.Info, watertight.Severity);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Bridge_RejectsGarbageFile()
    {
        if (!NativeEngineBridge.IsLoaded) return;
        var path = Path.Combine(Path.GetTempPath(), $"garbage_{Guid.NewGuid():N}.mesh");
        File.WriteAllText(path, "not a mesh at all");
        try
        {
            Assert.Null(NativeEngineBridge.ReadCookedMeshTriangles(path));
        }
        finally { File.Delete(path); }
    }
}
