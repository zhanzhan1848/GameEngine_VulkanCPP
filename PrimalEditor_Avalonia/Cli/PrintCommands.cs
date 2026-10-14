using System;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Printing;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 3d-printing S1 资产桥接 verbs（2026-10-15）：cooked .mesh → PrintMeshChecks/
/// MeshExporters。语义：模型单位=米（引擎坐标）；单位换算经 TransformToUnit
/// （米→目标，世界平移/缩放恒等——实例变换由用户在 DCC/场景侧考虑，S1 声明）；
/// STL 附旁附清单（单位/尺寸声明）；导出总写新文件（原件不变）。薄壁无参数
/// → NotChecked（未判定≠合格）。
/// </summary>
public sealed class PrintCommands
{
    private static (double[] positions, int[] indices) RequireTriangles(Guid assetGuid)
    {
        var info = AssetRegistry.GetByGuid(assetGuid)
            ?? throw CliError.NotFound($"资产 {assetGuid}", "asset.list 查看注册表");
        var cooked = CookedArtifactResolver.ResolveCookedPath(info);
        if (string.IsNullOrEmpty(cooked))
            throw CliError.NotFound(
                $"资产 '{info.FileName}' 没有 cooked .mesh", "先在内容浏览器导入/cook 该资产");
        var mesh = NativeEngineBridge.ReadCookedMeshTriangles(cooked);
        if (mesh == null)
            throw new CliError(CliErrorCodes.EngineError,
                $"cooked mesh 读取失败: {cooked}", "旧格式文件会被引擎一致拒绝——重新 cook 该资产");
        return mesh.Value;
    }

    [CliCommand("print.check",
        Description = "3D 打印制造几何检查（print S1）：cooked .mesh → 三角形汤 → 结构化报告。" +
                      "检查项：水密/边界孔洞/非流形边/退化面/绕向不一致/独立壳/有向体积朝向（内朝向" +
                      "警告）/包围盒尺寸。thinWallMeters 省略时薄壁=未判定（不是合格）。" +
                      "ValidationErrors=0 ≠ 打印合格——报告原文如此，不做绿色担保。",
        Example = """{"assetGuid":"…","thinWallMeters":0.001}""")]
    public object Check(Guid assetGuid, double? thinWallMeters = null)
    {
        if (thinWallMeters is < 0) throw CliError.InvalidParams("thinWallMeters 须 ≥ 0");
        var (positions, indices) = RequireTriangles(assetGuid);
        var report = PrintMeshChecks.Run(positions, indices, thinWallMeters * 1000.0);
        return new
        {
            triangles = indices.Length / 3,
            vertices = positions.Length / 3,
            sizeMeters = report.Size,
            hasErrors = report.HasErrors,
            findings = report.Findings.Select(f => new
            {
                severity = f.Severity.ToString().ToLowerInvariant(),
                check = f.Check,
                message = f.Message,
                value = f.Value,
            }).ToArray(),
        };
    }

    [CliCommand("print.export",
        Description = "导出打印文件（print S1）：cooked .mesh → STL（二进制+旁附清单）或 3MF" +
                      "（Core OPC，unit 显式）。unit: millimeter|centimeter|meter|inch（缺省 " +
                      "millimeter；模型单位=米，自动换算）。输出 <project>/Exports/<资产名>.<ext>" +
                      "——总写新文件，源资产不变。负缩放翻绕向语义在 TransformToUnit（恒等变换" +
                      "下不触发）。导出前自动跑一次检查并在结果附 findings 摘要。",
        Example = """{"assetGuid":"…","format":"3mf","unit":"millimeter"}""")]
    public object Export(Guid assetGuid, string format = "stl", string unit = "millimeter")
    {
        var (positions, indices) = RequireTriangles(assetGuid);
        var printUnit = unit.ToLowerInvariant() switch
        {
            "millimeter" or "mm" => PrintUnit.Millimeter,
            "centimeter" or "cm" => PrintUnit.Centimeter,
            "meter" or "m" => PrintUnit.Meter,
            "inch" => PrintUnit.Inch,
            _ => throw CliError.InvalidParams($"未知单位 '{unit}'", "millimeter/centimeter/meter/inch"),
        };
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        var projectDir = worldEditor.Project?.Path
            ?? throw CliError.NotReady("项目未加载", "先在编辑器中打开项目");

        var report = PrintMeshChecks.Run(positions, indices);
        var (tp, ti) = MeshExporters.TransformToUnit(
            positions, indices, new double[] { 0, 0, 0 }, new[] { 1.0, 1.0, 1.0 },
            PrintUnit.Meter, printUnit);
        var sizeInUnit = new[]
        {
            report.Size[0] * 1000.0 / PrintUnitFactors.ToMillimeters(printUnit),
            report.Size[1] * 1000.0 / PrintUnitFactors.ToMillimeters(printUnit),
            report.Size[2] * 1000.0 / PrintUnitFactors.ToMillimeters(printUnit),
        };
        var info = AssetRegistry.GetByGuid(assetGuid)!;
        var stem = Path.GetFileNameWithoutExtension(info.FileName);
        var dir = Path.Combine(projectDir, "Exports");
        Directory.CreateDirectory(dir);
        string path;
        if (format.ToLowerInvariant() == "3mf")
        {
            path = Path.Combine(dir, $"{stem}.3mf");
            MeshExporters.Write3Mf(path, tp, ti, printUnit, stem);
        }
        else if (format.ToLowerInvariant() == "stl")
        {
            path = Path.Combine(dir, $"{stem}.stl");
            MeshExporters.WriteStl(path, tp, ti, $"unit={unit}; source={info.FileName}");
            MeshExporters.WriteStlSidecar(path, printUnit, sizeInUnit, info.FileName);
        }
        else
        {
            throw CliError.InvalidParams($"未知格式 '{format}'", "stl 或 3mf");
        }
        return new
        {
            path,
            format = format.ToLowerInvariant(),
            unit = printUnit.ToString().ToLowerInvariant(),
            triangles = ti.Count / 3,
            sizeInUnit = sizeInUnit,
            checkHasErrors = report.HasErrors,
            checkFindings = report.Findings.Select(f => new
            {
                severity = f.Severity.ToString().ToLowerInvariant(),
                check = f.Check,
                message = f.Message,
            }).ToArray(),
        };
    }
}
