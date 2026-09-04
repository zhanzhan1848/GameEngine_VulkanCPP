using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using PrimalEditor_Avalonia.Editors.Pcg;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

// A6 增量：画布坐标持久化（"pos":[x,y] 嵌引擎 JSON）。引擎解析器按键名
// 取字段、天然忽略未知字段——坐标是纯编辑器数据，不进引擎图模型。
public sealed class PcgPositionPersistenceTests
{
    private static List<PcgNodeViewModel> MakeNodes(params (double x, double y)[] locations)
    {
        return locations.Select((loc, i) =>
        {
            var vm = new PcgNodeViewModel((uint)i, "NoiseField");
            vm.Location = new Point(loc.x, loc.y);
            return vm;
        }).ToList();
    }

    [Fact]
    public void MergePositions_WritesPosIntoEachNode()
    {
        var json = "{\"nodes\":[{\"type\":\"NoiseField\"},{\"type\":\"FieldScatter\"}],\"connections\":[]}";
        var nodes = MakeNodes((120.5, 80.25), (300, 160));

        var merged = PcgEditorViewModel.MergePositions(json, nodes);

        Assert.Contains("\"pos\":[120.5,80.2", merged.Replace(" ", ""));
        Assert.Contains("\"pos\":[300,160]", merged.Replace(" ", ""));
    }

    [Fact]
    public void MergePositions_FewerNodesThanJson_TouchesOnlyCanvasNodes()
    {
        var json = "{\"nodes\":[{\"type\":\"A\"},{\"type\":\"B\"},{\"type\":\"C\"}]}";
        var nodes = MakeNodes((10, 20));

        var merged = PcgEditorViewModel.MergePositions(json, nodes);

        Assert.Contains("\"pos\":[10,20]", merged.Replace(" ", ""));
        // 未覆盖节点保持无 pos 字段——恰好 1 处 "pos"（只覆盖画布上的 1 个节点）
        Assert.Equal(1, merged.Split("\"pos\"").Length - 1);
    }

    [Fact]
    public void MergePositions_MalformedJson_ReturnsOriginal()
    {
        const string bad = "{not json";
        Assert.Equal(bad, PcgEditorViewModel.MergePositions(bad, MakeNodes((1, 2))));
    }

    [Fact]
    public void MergePositions_NoNodesArray_ReturnsOriginal()
    {
        const string json = "{\"mesh_slots\":[]}";
        Assert.Equal(json, PcgEditorViewModel.MergePositions(json, MakeNodes((1, 2))));
    }

    [Fact]
    public void TryReadPos_RoundTripsThroughMerge()
    {
        var json = "{\"nodes\":[{\"type\":\"NoiseField\"}]}";
        var nodes = MakeNodes((77.7, 42.3));
        var merged = PcgEditorViewModel.MergePositions(json, nodes);

        using var doc = System.Text.Json.JsonDocument.Parse(merged);
        var pos = PcgEditorViewModel.TryReadPos(doc.RootElement.GetProperty("nodes")[0]);

        Assert.NotNull(pos);
        Assert.Equal(77.7, pos.Value.X, 1);
        Assert.Equal(42.3, pos.Value.Y, 1);
    }

    [Fact]
    public void TryReadPos_MissingOrMalformed_ReturnsNull()
    {
        using var noPos = System.Text.Json.JsonDocument.Parse("{\"type\":\"A\"}");
        Assert.Null(PcgEditorViewModel.TryReadPos(noPos.RootElement));

        using var malformed = System.Text.Json.JsonDocument.Parse("{\"pos\":[1]}");
        Assert.Null(PcgEditorViewModel.TryReadPos(malformed.RootElement));

        using var nonNumeric = System.Text.Json.JsonDocument.Parse("{\"pos\":[\"a\",2]}");
        Assert.Null(PcgEditorViewModel.TryReadPos(nonNumeric.RootElement));
    }
}
