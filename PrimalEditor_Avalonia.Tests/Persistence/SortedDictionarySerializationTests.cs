using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject.Documents;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

// A8 场景 JSON 排序稳定化：Dictionary<string,T> 字段写盘键序固定为 Ordinal
// 排序。两个用户以不同插入顺序做出相同逻辑修改，必须产生字节一致的文件
//（否则 git diff 全是噪音）。
public sealed class SortedDictionarySerializationTests
{
    private static string Serialize<T>(T value)
        => JsonSerializer.Serialize(value, SceneDocument.JsonOptions);

    [Fact]
    public void MaterialOverrideParameters_DifferentInsertionOrder_SameJson()
    {
        var a = new MaterialOverrideSlot();
        a.Parameters["roughness"] = new MaterialParameterOverride { Scalar = 0.5f };
        a.Parameters["base_color"] = new MaterialParameterOverride { Scalar = 1f };

        var b = new MaterialOverrideSlot();
        b.Parameters["base_color"] = new MaterialParameterOverride { Scalar = 1f };
        b.Parameters["roughness"] = new MaterialParameterOverride { Scalar = 0.5f };

        Assert.Equal(Serialize(a), Serialize(b));
    }

    [Fact]
    public void PrefabOverrides_DifferentInsertionOrder_SameJson()
    {
        var a = new PrefabInstanceComponent();
        a.Overrides["Wall"] = "{\"PositionX\":1}";
        a.Overrides["Floor"] = "{\"PositionX\":0}";

        var b = new PrefabInstanceComponent();
        b.Overrides["Floor"] = "{\"PositionX\":0}";
        b.Overrides["Wall"] = "{\"PositionX\":1}";

        Assert.Equal(Serialize(a), Serialize(b));
        // 键序按 Ordinal 排序："Floor" < "Wall"
        var json = Serialize(a);
        Assert.True(
            json.IndexOf("\"Floor\"", System.StringComparison.Ordinal)
                < json.IndexOf("\"Wall\"", System.StringComparison.Ordinal),
            $"keys not in sorted order: {json}");
    }

    [Fact]
    public void ScriptParameters_DifferentInsertionOrder_SameJson()
    {
        var a = new RotateScriptComponent();
        a.Parameters["speed"] = "2";
        a.Parameters["axis"] = "y";

        var b = new RotateScriptComponent();
        b.Parameters["axis"] = "y";
        b.Parameters["speed"] = "2";

        Assert.Equal(Serialize(a), Serialize(b));
    }

    [Fact]
    public void SortedJson_RoundTrips_BackToEqualDictionary()
    {
        var original = new PrefabInstanceComponent();
        original.Overrides["B"] = "2";
        original.Overrides["A"] = "1";

        var json = Serialize(original);
        var restored = JsonSerializer.Deserialize<PrefabInstanceComponent>(
            json, SceneDocument.JsonOptions);

        Assert.NotNull(restored);
        Assert.Equal(original.Overrides.Count, restored!.Overrides.Count);
        Assert.Equal("1", restored.Overrides["A"]);
        Assert.Equal("2", restored.Overrides["B"]);
    }
}
