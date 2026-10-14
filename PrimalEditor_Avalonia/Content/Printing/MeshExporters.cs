using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace PrimalEditor_Avalonia.Content.Printing;

/// <summary>
/// STL/3MF 导出（3d-printing S1，2026-10-15）。输入模型空间三角形汤 + 世界
/// 变换（位置 + 三轴缩放；负缩放自动翻绕向保持外法线）+ 目标单位/尺寸声明。
/// 原件不变——导出总是生成新文件；STL 无单位语义，尺寸/单位写入旁附清单。
/// </summary>
public static class MeshExporters
{
    /// <summary>应用世界变换后的三角形汤（毫米）。negativeScale 翻转绕向。</summary>
    public static (List<double> positions, List<int> triangles) TransformToUnit(
        IReadOnlyList<double> positions, IReadOnlyList<int> triangles,
        double[] worldPosition, double[] worldScale,
        PrintUnit modelUnit, PrintUnit exportUnit)
    {
        if (worldPosition is not { Length: 3 } || worldScale is not { Length: 3 })
            throw new ArgumentException("worldPosition/worldScale 须为 3 元素");
        var factor = PrintUnitFactors.ToMillimeters(modelUnit)
                   / PrintUnitFactors.ToMillimeters(exportUnit);
        var negative = worldScale[0] * worldScale[1] * worldScale[2] < 0;

        var outPositions = new List<double>(positions.Count);
        for (int i = 0; i + 2 < positions.Count; i += 3)
        {
            outPositions.Add((positions[i] * worldScale[0] + worldPosition[0]) * factor);
            outPositions.Add((positions[i + 1] * worldScale[1] + worldPosition[1]) * factor);
            outPositions.Add((positions[i + 2] * worldScale[2] + worldPosition[2]) * factor);
        }
        var outTriangles = new List<int>(triangles.Count);
        for (int t = 0; t < triangles.Count; t += 3)
        {
            if (negative)
                outTriangles.AddRange(new[] { triangles[t], triangles[t + 2], triangles[t + 1] });
            else
                outTriangles.AddRange(new[] { triangles[t], triangles[t + 1], triangles[t + 2] });
        }
        return (outPositions, outTriangles);
    }

    /// <summary>二进制 STL（80 字节头 + 三角数 + 每三角 50 字节：法线 + 3 顶点 + 属性 0）。
    /// 法线由顶点叉积计算（右手系外法线，退化面写 0）。</summary>
    public static void WriteStl(
        string path, IReadOnlyList<double> positions, IReadOnlyList<int> triangles,
        string headerNote)
    {
        using var fs = new FileStream(path, FileMode.Create, FileAccess.Write);
        using var w = new BinaryWriter(fs);
        var header = Encoding.ASCII.GetBytes(
            ("Primal STL export — " + headerNote).PadRight(80)[..80]);
        w.Write(header);
        w.Write((uint)(triangles.Count / 3));
        for (int t = 0; t < triangles.Count; t += 3)
        {
            var a = triangles[t] * 3; var b = triangles[t + 1] * 3; var c = triangles[t + 2] * 3;
            var abx = positions[b] - positions[a]; var aby = positions[b + 1] - positions[a + 1]; var abz = positions[b + 2] - positions[a + 2];
            var acx = positions[c] - positions[a]; var acy = positions[c + 1] - positions[a + 1]; var acz = positions[c + 2] - positions[a + 2];
            var nx = aby * acz - abz * acy;
            var ny = abz * acx - abx * acz;
            var nz = abx * acy - aby * acx;
            var len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-20) { nx /= len; ny /= len; nz /= len; } else { nx = ny = nz = 0; }
            w.Write((float)nx); w.Write((float)ny); w.Write((float)nz);
            w.Write((float)positions[a]); w.Write((float)positions[a + 1]); w.Write((float)positions[a + 2]);
            w.Write((float)positions[b]); w.Write((float)positions[b + 1]); w.Write((float)positions[b + 2]);
            w.Write((float)positions[c]); w.Write((float)positions[c + 1]); w.Write((float)positions[c + 2]);
            w.Write((ushort)0);
        }
    }

    /// <summary>3MF Core 最小包（OPC zip）：[Content_Types].xml + _rels/.rels +
    /// 3D/3dmodel.model（unit 显式、单一 mesh 对象）。ZipArchive 负责 zip 结构。</summary>
    public static void Write3Mf(
        string path, IReadOnlyList<double> positions, IReadOnlyList<int> triangles,
        PrintUnit unit, string objectName)
    {
        using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
        var contentTypes = zip.CreateEntry("[Content_Types].xml");
        WriteText(contentTypes,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">" +
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>" +
            "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>" +
            "</Types>");
        var rels = zip.CreateEntry("_rels/.rels");
        WriteText(rels,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
            "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" " +
            "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>" +
            "</Relationships>");

        var unitName = unit switch
        {
            PrintUnit.Millimeter => "millimeter",
            PrintUnit.Centimeter => "centimeter",
            PrintUnit.Meter => "meter",
            PrintUnit.Inch => "inch",
            _ => "millimeter",
        };
        var sb = new StringBuilder(positions.Count * 12);
        sb.Append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
        sb.Append("<model unit=\"").Append(unitName)
          .Append("\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">");
        sb.Append("<resources><object id=\"1\" type=\"model\"><mesh>");
        sb.Append("<vertices>");
        for (int i = 0; i + 2 < positions.Count; i += 3)
            sb.Append("<vertex x=\"").Append(FormattableString.Invariant($"{positions[i]:R}"))
              .Append("\" y=\"").Append(FormattableString.Invariant($"{positions[i + 1]:R}"))
              .Append("\" z=\"").Append(FormattableString.Invariant($"{positions[i + 2]:R}"))
              .Append("\"/>");
        sb.Append("</vertices><triangles>");
        for (int t = 0; t < triangles.Count; t += 3)
            sb.Append("<triangle v1=\"").Append(triangles[t])
              .Append("\" v2=\"").Append(triangles[t + 1])
              .Append("\" v3=\"").Append(triangles[t + 2]).Append("\"/>");
        sb.Append("</triangles></mesh></object></resources>");
        sb.Append("<build><item objectid=\"1\"/></build>");
        sb.Append("</model>");
        var model = zip.CreateEntry("3D/3dmodel.model");
        WriteText(model, sb.ToString());
        return;

        static void WriteText(ZipArchiveEntry entry, string text)
        {
            using var s = entry.Open();
            using var sw = new StreamWriter(s, new UTF8Encoding(false));
            sw.Write(text);
        }
    }

    /// <summary>STL 旁附清单（STL 无单位语义——尺寸/单位/源信息在此声明）。</summary>
    public static void WriteStlSidecar(string stlPath, PrintUnit unit,
        double[] sizeInUnit, string sourceNote)
    {
        var sidecar = Path.ChangeExtension(stlPath, ".stl.txt");
        File.WriteAllText(sidecar, new StringBuilder()
            .AppendLine("# Primal STL export manifest")
            .AppendLine($"unit: {unit}")
            .AppendLine($"size: {sizeInUnit[0]:R} x {sizeInUnit[1]:R} x {sizeInUnit[2]:R}")
            .AppendLine($"source: {sourceNote}")
            .AppendLine("note: STL carries no unit semantics — this manifest is the declaration of record.")
            .ToString());
    }
}
