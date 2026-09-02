using System;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// Phase 1 补齐 verbs 的文档模型语义：scene.new/save/load、asset.list、camera.set、
/// material.set-param。全部走真实 WorldEditorViewModel + UndoRedo 栈（原生桥未加载，
/// 同步器与引擎推送为 no-op/NOT_READY 路径）。
/// </summary>
public sealed class CliSceneComponentCommandTests : IDisposable
{
    private readonly Project _project;
    private readonly Scene _scene;
    private readonly WorldEditorViewModel _worldEditor;
    private readonly string _projectDir;

    public CliSceneComponentCommandTests()
    {
        _projectDir = Path.Combine(Path.GetTempPath(), "cli-scene-test-" + Guid.NewGuid().ToString("N"));
        _project = Project.CreateNew("CliSceneTest", _projectDir);
        _scene = new Scene("Main");
        _project.Scenes.Add(_scene);
        _worldEditor = new WorldEditorViewModel { Project = _project, ActiveScene = _scene };
        EditorServices.WorldEditorOverride = _worldEditor;
    }

    public void Dispose()
    {
        EditorServices.WorldEditorOverride = null;
        try { Directory.Delete(_projectDir, recursive: true); } catch { }
    }

    private static CliResponse Invoke(string verb, object? params_ = null)
    {
        var registry = new Primal.CliFramework.CliCommandRegistry();
        registry.Register(new EditorCommands());
        registry.Register(new EntityCommands());
        registry.Register(new SceneCommands());
        registry.Register(new ComponentCommands());
        registry.Register(new WorkflowCommands(registry));
        return registry.InvokeAsync(new CliRequest
        {
            Id = "t",
            Verb = verb,
            Params = System.Text.Json.JsonSerializer.SerializeToElement(params_ ?? new { }),
        }).GetAwaiter().GetResult();
    }

    // === camera.set ===

    [Fact]
    public void SetCamera_FovAndPlanes_UpdateComponentAndSupportUndo()
    {
        var camera = new GameEntity("Cam");
        camera.Components.Add(new Transform());
        camera.Components.Add(new CameraComponent());
        _scene.Entities.Add(camera);
        var guid = camera.EntityGuid.Value;

        var response = Invoke("camera.set", new
        {
            entityGuid = guid,
            fov = 45,
            nearPlane = 0.5f,
            farPlane = 800,
        });
        Assert.True(response.Ok, response.Error?.Message);
        Assert.Equal(45f, camera.GetComponent<CameraComponent>()!.FieldOfViewDegrees);
        Assert.Equal(0.5f, camera.GetComponent<CameraComponent>()!.NearPlane);
        Assert.Equal(800f, camera.GetComponent<CameraComponent>()!.FarPlane);

        Assert.True(Invoke("editor.undo").Ok);
        Assert.Equal(60f, camera.GetComponent<CameraComponent>()!.FieldOfViewDegrees); // 默认值恢复
        Assert.True(Invoke("editor.redo").Ok);
        Assert.Equal(45f, camera.GetComponent<CameraComponent>()!.FieldOfViewDegrees);
    }

    [Fact]
    public void SetCamera_CrossValueNearFar_DoesNotThrowMidState()
    {
        var camera = new GameEntity("Cam");
        camera.Components.Add(new Transform());
        camera.Components.Add(new CameraComponent());
        _scene.Entities.Add(camera);

        // 500 → 5 的远平面迁移伴随近平面抬高：内部 setter 强制 near<far，
        // 实现必须整对校验后按安全顺序落，否则中间态抛 ArgumentOutOfRangeException。
        var response = Invoke("camera.set", new { entityGuid = camera.EntityGuid.Value, nearPlane = 1, farPlane = 5 });
        Assert.True(response.Ok, response.Error?.Message);
        Assert.Equal(1f, camera.GetComponent<CameraComponent>()!.NearPlane);
        Assert.Equal(5f, camera.GetComponent<CameraComponent>()!.FarPlane);
    }

    [Fact]
    public void SetCamera_InvalidFov_FailsWithInvalidParams()
    {
        var camera = new GameEntity("Cam");
        camera.Components.Add(new Transform());
        camera.Components.Add(new CameraComponent());
        _scene.Entities.Add(camera);

        var response = Invoke("camera.set", new { entityGuid = camera.EntityGuid.Value, fov = 200 });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
        Assert.Equal(60f, camera.GetComponent<CameraComponent>()!.FieldOfViewDegrees); // 未被修改
    }

    [Fact]
    public void SetCamera_EntityWithoutCameraComponent_ReturnsNotFound()
    {
        var prop = new GameEntity("Prop");
        prop.Components.Add(new Transform());
        _scene.Entities.Add(prop);

        var response = Invoke("camera.set", new { entityGuid = prop.EntityGuid.Value, fov = 45 });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.NotFound, response.Error!.Code);
        Assert.Contains("entity.create kind=Camera", response.Error.Hint, StringComparison.Ordinal);
    }

    // === material.set-param ===

    [Fact]
    public void SetMaterialParam_RangeValidationFails_WithoutTouchingEngine()
    {
        var prop = new GameEntity("Prop");
        prop.Components.Add(new Transform());
        _scene.Entities.Add(prop);

        var response = Invoke("material.set-param", new { entityGuid = prop.EntityGuid.Value, roughness = 1.5f });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
    }

    // === scene.* ===

    [Fact]
    public void SceneNew_CreatesAndActivatesSceneWithDefaultSun()
    {
        var response = Invoke("scene.new", new { name = "夜城" });
        Assert.True(response.Ok, response.Error?.Message);
        var data = System.Text.Json.JsonSerializer.Deserialize<System.Text.Json.JsonElement>(
            System.Text.Json.JsonSerializer.Serialize(response.Data));

        Assert.Equal(2, _worldEditor.Scenes.Count);
        Assert.Equal("夜城", _worldEditor.ActiveScene!.Name);
        // 新场景自带默认平行光（AddScene 与 UI 一致）
        Assert.Contains(_worldEditor.ActiveScene.Entities,
            e => e.GetComponent<LightComponent>() != null);
        Assert.Equal("夜城", data.GetProperty("sceneName").GetString());
        Assert.True(data.GetProperty("created").GetBoolean());
    }

    [Fact]
    public void SceneSave_WritesProjectAndSceneFiles()
    {
        var response = Invoke("scene.save");
        Assert.True(response.Ok, response.Error?.Message);
        Assert.True(File.Exists(Path.Combine(_projectDir, "CliSceneTest.primal")),
            "项目文件应落盘");
    }

    [Fact]
    public void SceneLoad_MissingFile_ReturnsNotFound()
    {
        var response = Invoke("scene.load", new { path = "Scenes/does-not-exist.scene" });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.NotFound, response.Error!.Code);
    }

    // === asset.list ===

    [Fact]
    public void AssetList_UnknownType_FailsWithHint()
    {
        var response = Invoke("asset.list", new { type = "NotAType" });
        Assert.False(response.Ok);
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
        Assert.StartsWith("有效取值", response.Error.Hint, StringComparison.Ordinal);
    }

    [Fact]
    public void AssetList_LimitBounds()
    {
        Assert.Equal(CliErrorCodes.InvalidParams, Invoke("asset.list", new { limit = 0 }).Error!.Code);
        Assert.Equal(CliErrorCodes.InvalidParams, Invoke("asset.list", new { limit = 501 }).Error!.Code);
        var ok = Invoke("asset.list", new { limit = 10 });
        Assert.True(ok.Ok, ok.Error?.Message);
    }

    // === workflow.run / docs.search（Phase 3）===

    [Fact]
    public void WorkflowRun_StopOnError_ReportsCompletedSteps()
    {
        // 第 1 步应成功（camera.set 合法），第 2 步非法 guid → NOT_FOUND → stop-on-error。
        var camera = new GameEntity("Cam");
        camera.Components.Add(new Transform());
        camera.Components.Add(new CameraComponent());
        _scene.Entities.Add(camera);

        var response = Invoke("workflow.run", new
        {
            steps = new object[]
            {
                new { verb = "camera.set", @params = new { entityGuid = camera.EntityGuid.Value, fov = 90 } },
                new { verb = "camera.set", @params = new { entityGuid = "00000000-0000-0000-0000-000000000000", fov = 30 } },
                new { verb = "editor.ping" },
            },
        });

        Assert.True(response.Ok, response.Error?.Message); // workflow 命令本身成功，data 报告 stop-on-error 状态
        var data = System.Text.Json.JsonSerializer.Deserialize<System.Text.Json.JsonElement>(
            System.Text.Json.JsonSerializer.Serialize(response.Data));
        Assert.False(data.GetProperty("completed").GetBoolean());
        Assert.Equal(2, data.GetProperty("executed").GetInt32());
        Assert.Equal(1, data.GetProperty("failedAt").GetInt32());
        Assert.Equal(90f, camera.GetComponent<CameraComponent>()!.FieldOfViewDegrees); // 第 1 步已生效
    }

    [Fact]
    public void WorkflowRun_AllStepsSucceed_ReportsCompleted()
    {
        var response = Invoke("workflow.run", new
        {
            steps = new object[]
            {
                new { verb = "editor.ping" },
                new { verb = "scene.tree" },
            },
        });
        Assert.True(response.Ok, response.Error?.Message);
        var data = System.Text.Json.JsonSerializer.Deserialize<System.Text.Json.JsonElement>(
            System.Text.Json.JsonSerializer.Serialize(response.Data));
        Assert.True(data.GetProperty("completed").GetBoolean());
        Assert.Equal(2, data.GetProperty("executed").GetInt32());
    }

    [Fact]
    public void WorkflowRun_UnknownVerb_ReturnsNotFound()
    {
        var response = Invoke("workflow.run", new
        {
            steps = new object[] { new { verb = "does.not.exist" } },
        });
        Assert.Equal(CliErrorCodes.NotFound, response.Error!.Code);
        Assert.Contains("editor.schema", response.Error.Hint, StringComparison.Ordinal);
    }

    [Fact]
    public void DocsSearch_EmptyQuery_ReturnsInvalidParams()
    {
        var response = Invoke("docs.search", new { query = "" });
        Assert.Equal(CliErrorCodes.InvalidParams, response.Error!.Code);
    }

    [Fact]
    public void DocsSearch_LimitBounds()
    {
        Assert.Equal(CliErrorCodes.InvalidParams, Invoke("docs.search", new { query = "x", limit = 0 }).Error!.Code);
        Assert.Equal(CliErrorCodes.InvalidParams, Invoke("docs.search", new { query = "x", limit = 101 }).Error!.Code);
    }
}
