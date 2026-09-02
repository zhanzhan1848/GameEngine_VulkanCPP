using System;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// Phase 0 CLI 种子命令的纯托管部分：scene.tree 的实体/组件序列化。
/// 不需要引擎/视口，直接构造文档模型断言 JSON 形态（guid 小写、字段名、
/// vec3 数组布局）——这是 LLM 消费契约，漂移即测试红。
/// </summary>
public class CliSceneTreeSerializationTests
{
    private static JsonElement Serialize(object payload) =>
        JsonSerializer.SerializeToElement(payload, new JsonSerializerOptions());

    [Fact]
    public void DescribeEntity_EmitsLowercaseGuidNameEnabledAndComponents()
    {
        var entity = new GameEntity("Sun Light") { IsEnabled = true };
        entity.Components.Add(new Transform
        {
            PositionX = 1.5f, PositionY = 2f, PositionZ = -3.25f,
            RotationX = 0f, RotationY = 90f, RotationZ = 0f,
        });

        var json = Serialize(EditorCommands.DescribeEntity(entity));

        Assert.Equal(entity.EntityGuid.Value.ToString("D").ToLowerInvariant(),
                     json.GetProperty("entityGuid").GetString());
        Assert.Equal("Sun Light", json.GetProperty("name").GetString());
        Assert.True(json.GetProperty("isEnabled").GetBoolean());
        Assert.Equal(1, json.GetProperty("components").GetArrayLength());
    }

    [Fact]
    public void DescribeComponent_TransformUsesMeterDegreeRightHandArrays()
    {
        var transform = new Transform
        {
            PositionX = 1f, PositionY = 0f, PositionZ = 3f,
            RotationX = 45f, RotationY = 0f, RotationZ = 180f,
            ScaleX = 2f, ScaleY = 2f, ScaleZ = 2f,
        };

        var json = Serialize(EditorCommands.DescribeComponent(transform));

        Assert.Equal("Transform", json.GetProperty("type").GetString());
        Assert.Equal(new[] { 1f, 0f, 3f }, json.GetProperty("position").EnumerateArray().Select(v => v.GetSingle()).ToArray());
        Assert.Equal(new[] { 45f, 0f, 180f }, json.GetProperty("rotationEuler").EnumerateArray().Select(v => v.GetSingle()).ToArray());
        Assert.Equal(new[] { 2f, 2f, 2f }, json.GetProperty("scale").EnumerateArray().Select(v => v.GetSingle()).ToArray());
    }

    [Fact]
    public void DescribeComponent_LightCarriesTypeColorIntensity()
    {
        var light = new LightComponent
        {
            LightType = LightType.Point,
            ColorR = 1f, ColorG = 0.5f, ColorB = 0.25f,
            Intensity = 8f,
            Range = 30f,
        };

        var json = Serialize(EditorCommands.DescribeComponent(light));

        Assert.Equal("Light", json.GetProperty("type").GetString());
        Assert.Equal("Point", json.GetProperty("lightType").GetString());
        Assert.Equal(8f, json.GetProperty("intensity").GetSingle());
        Assert.Equal(3, json.GetProperty("color").GetArrayLength());
    }

    [Fact]
    public void Registry_IncludesEditorSchemaVersionAndSeedVerbs()
    {
        // 与 CliHost.Start 相同的装配路径（不启动 socket）。
        var registry = new Primal.CliFramework.CliCommandRegistry();
        registry.Register(new EditorCommands());

        var verbs = registry.Commands.Select(c => c.Verb).ToArray();
        Assert.Contains("editor.ping", verbs);
        Assert.Contains("editor.schema", verbs);
        Assert.Contains("scene.tree", verbs);
        Assert.Contains("editor.screenshot", verbs);
        Assert.Equal(16, registry.ComputeVersion().Length);
    }
}
