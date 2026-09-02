using System;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// Phase 1 编辑 verbs 的文档模型语义：创建/删除/复制/变换/命名/灯光/undo。
/// 全部走真实 WorldEditorViewModel + UndoRedo 栈（原生桥未加载，同步器为 no-op），
/// 断言"改了什么、引擎推送路径是否被走到由 UI 侧既有测试覆盖"之外的编辑语义。
/// </summary>
public sealed class CliEntityCommandTests : IDisposable
{
    private readonly Project _project;
    private readonly Scene _scene;
    private readonly WorldEditorViewModel _worldEditor;

    public CliEntityCommandTests()
    {
        _project = Project.CreateNew("CliTest", "/tmp/cli-test-project");
        _scene = new Scene("Main");
        _project.Scenes.Add(_scene);
        _worldEditor = new WorldEditorViewModel { Project = _project, ActiveScene = _scene };
        EditorServices.WorldEditorOverride = _worldEditor;
    }

    public void Dispose() => EditorServices.WorldEditorOverride = null;

    private static CliRequest Req(string verb, object? params_ = null) => new()
    {
        Id = "t",
        Verb = verb,
        Params = System.Text.Json.JsonSerializer.SerializeToElement(params_ ?? new { }),
    };

    private static CliResponse Invoke(string verb, object? params_ = null)
    {
        var registry = new Primal.CliFramework.CliCommandRegistry();
        registry.Register(new EditorCommands());
        registry.Register(new EntityCommands());
        return registry.InvokeAsync(Req(verb, params_)).GetAwaiter().GetResult();
    }

    private static JsonElementAssistant Data(CliResponse response)
    {
        Assert.True(response.Ok, $"expected ok, got {response.Error?.Code}: {response.Error?.Message}");
        return new JsonElementAssistant(response.Data!.Value);
    }

    /// <summary>JsonElement 的轻量取值助手（索引器 + GetXxx 转发）。</summary>
    private readonly struct JsonElementAssistant
    {
        private readonly System.Text.Json.JsonElement _element;
        public JsonElementAssistant(System.Text.Json.JsonElement element) => _element = element;
        public JsonElementAssistant this[string name] => new(_element.GetProperty(name));
        public JsonElementAssistant this[int index] => new(_element[index]);
        public string Str() => _element.GetString()!;
        public float Num() => _element.GetSingle();
        public bool Bool() => _element.GetBoolean();
        public float[] Vec() => _element.EnumerateArray().Select(v => v.GetSingle()).ToArray();
    }

    [Fact]
    public void Create_SphereWithNameAndPosition_LandsInSceneAsOneUndoStep()
    {
        var data = Data(Invoke("entity.create", new
        {
            kind = "Sphere",
            name = "测试球",
            position = new[] { 1f, 2f, 3f },
        }));

        Assert.Equal("测试球", data["name"].Str());
        Assert.Equal(new[] { 1f, 2f, 3f }, data["components"][0]["position"].Vec());
        Assert.Equal(1, _scene.Entities.Count);

        // 一次 undo 整体撤销（创建+命名+位置不是三个动作）
        var undo = Data(Invoke("editor.undo"));
        Assert.True(undo["undone"].Bool());
        Assert.Equal(0, _scene.Entities.Count);
    }

    [Fact]
    public void Create_UnknownKind_FailsWithInvalidParams()
    {
        var response = Invoke("entity.create", new { kind = "Dragon" });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
        Assert.Contains("Sphere", response.Error.Hint, StringComparison.Ordinal);
    }

    [Fact]
    public void SetTransform_PartialUpdate_KeepsOtherFieldsAndSupportsUndoRedo()
    {
        var created = Data(Invoke("entity.create", new
        {
            kind = "Box",
            position = new[] { 0f, 0f, 0f },
            rotationEuler = new[] { 45f, 0f, 0f },
            scale = new[] { 2f, 2f, 2f },
        }));
        var guid = created["entityGuid"].Str();

        var moved = Data(Invoke("entity.set-transform", new
        {
            entityGuid = guid,
            position = new[] { 5f, 6f, 7f },
        }));

        Assert.Equal(new[] { 5f, 6f, 7f }, moved["position"].Vec());
        Assert.Equal(new[] { 45f, 0f, 0f }, moved["rotationEuler"].Vec()); // 未传字段保持
        Assert.Equal(new[] { 2f, 2f, 2f }, moved["scale"].Vec());

        Invoke("editor.undo");
        var entity = _scene.Entities.Single();
        Assert.Equal(0f, entity.GetTransform()!.PositionX);

        Invoke("editor.redo");
        Assert.Equal(5f, entity.GetTransform()!.PositionX);
    }

    [Fact]
    public void SetTransform_ZeroScale_RejectedWithoutMutation()
    {
        var created = Data(Invoke("entity.create", new { kind = "Empty" }));
        var guid = created["entityGuid"].Str();

        var response = Invoke("entity.set-transform", new
        {
            entityGuid = guid,
            scale = new[] { 0f, 1f, 1f },
        });

        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
        Assert.Equal(1f, _scene.Entities.Single().GetTransform()!.ScaleX);
    }

    [Fact]
    public void SetName_ChangesAndUndoes()
    {
        var created = Data(Invoke("entity.create", new { kind = "Empty", name = "旧名" }));
        var guid = created["entityGuid"].Str();

        var renamed = Data(Invoke("entity.set-name", new { entityGuid = guid, name = "新名" }));
        Assert.Equal("旧名", renamed["oldName"].Str());
        Assert.Equal("新名", _scene.Entities.Single().Name);

        Invoke("editor.undo");
        Assert.Equal("旧名", _scene.Entities.Single().Name);
    }

    [Fact]
    public void Delete_RemovesEntityAndUndoRestoresIt()
    {
        var created = Data(Invoke("entity.create", new { kind = "Light" }));
        var guid = created["entityGuid"].Str();

        Data(Invoke("entity.delete", new { entityGuid = guid }));
        Assert.Empty(_scene.Entities);

        Invoke("editor.undo");
        Assert.Single(_scene.Entities);

        Invoke("editor.redo");
        Assert.Empty(_scene.Entities);
    }

    [Fact]
    public void SetLight_UpdatesComponentValidatesAndUndoes()
    {
        var created = Data(Invoke("entity.create", new { kind = "Light" }));
        var guid = created["entityGuid"].Str();

        var lit = Data(Invoke("light.set", new
        {
            entityGuid = guid,
            intensity = 8f,
            color = new[] { 1f, 0.5f, 0.25f },
        }));

        Assert.Equal(8f, lit["intensity"].Num());
        Assert.Equal(new[] { 1f, 0.5f, 0.25f }, lit["color"].Vec());

        // 负强度 → INVALID_PARAMS 且不落盘
        var bad = Invoke("light.set", new { entityGuid = guid, intensity = -1f });
        Assert.Equal(CliErrorCodes.InvalidParams, bad.Error!.Code);
        Assert.Equal(8f, _scene.Entities.Single().GetComponent<LightComponent>()!.Intensity);

        Invoke("editor.undo"); // 撤销灯光修改（undo 栈顶就是这次 light.set）
        var light = _scene.Entities.Single().GetComponent<LightComponent>()!;
        Assert.Equal(1f, light.Intensity);
        Assert.Equal(1f, light.ColorG);

        Invoke("editor.redo");
        Assert.Equal(8f, _scene.Entities.Single().GetComponent<LightComponent>()!.Intensity);

        // 非灯光实体 → NOT_FOUND
        var nonLight = Data(Invoke("entity.create", new { kind = "Empty" }));
        var noLight = Invoke("light.set", new { entityGuid = nonLight["entityGuid"].Str(), intensity = 2f });
        Assert.Equal(CliErrorCodes.NotFound, noLight.Error!.Code);
    }

    [Fact]
    public void Get_And_Delete_UnknownGuid_ReturnNotFound()
    {
        var missing = Guid.NewGuid().ToString("D");

        var get = Invoke("entity.get", new { entityGuid = missing });
        Assert.Equal(CliErrorCodes.NotFound, get.Error!.Code);
        Assert.Contains("scene.tree", get.Error.Hint, StringComparison.Ordinal);

        var delete = Invoke("entity.delete", new { entityGuid = missing });
        Assert.Equal(CliErrorCodes.NotFound, delete.Error!.Code);
    }

    [Fact]
    public void Undo_OnEmptyStack_ReturnsOkWithUndoneFalse()
    {
        var data = Data(Invoke("editor.undo"));
        Assert.False(data["undone"].Bool());
    }
}
