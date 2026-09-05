using System;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Sequence;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// sequence.* 命令面的文档模型语义（Part A）：生命周期（.pseq 落盘/回读）、
/// 轨道编辑（undo 往返）、set-frame 的求值→应用链。全部走真实
/// WorldEditorViewModel + UndoRedo 栈（原生桥未加载，同步器 no-op）。
/// play/render 依赖 Avalonia UI 线程与 3D 视口，只断言其守卫路径。
/// </summary>
public sealed class CliSequenceCommandTests : IDisposable
{
    private readonly Project _project;
    private readonly Scene _scene;
    private readonly WorldEditorViewModel _worldEditor;

    public CliSequenceCommandTests()
    {
        _project = Project.CreateNew("CliSeqTest", Path.Combine(Path.GetTempPath(), "cli-seq-test-" + Guid.NewGuid().ToString("N")));
        _scene = new Scene("Main");
        _project.Scenes.Add(_scene);
        _worldEditor = new WorldEditorViewModel { Project = _project, ActiveScene = _scene };
        EditorServices.WorldEditorOverride = _worldEditor;
    }

    public void Dispose()
    {
        EditorServices.WorldEditorOverride = null;
        SequenceEditorState.Close();
        try { Directory.Delete(_project.Path, recursive: true); } catch { }
    }

    private static CliRequest Req(string verb, object? params_ = null) => new()
    {
        Id = "t",
        Verb = verb,
        Params = System.Text.Json.JsonSerializer.SerializeToElement(params_ ?? new { }),
    };

    private static CliResponse Invoke(string verb, object? params_ = null)
    {
        var registry = new CliCommandRegistry();
        registry.Register(new EditorCommands());
        registry.Register(new EntityCommands());
        registry.Register(new SequenceCommands());
        return registry.InvokeAsync(Req(verb, params_)).GetAwaiter().GetResult();
    }

    private static JsonElementAssistant Data(CliResponse response)
    {
        Assert.True(response.Ok, $"expected ok, got {response.Error?.Code}: {response.Error?.Message}");
        return new JsonElementAssistant(response.Data!.Value);
    }


    private readonly struct JsonElementAssistant
    {
        private readonly System.Text.Json.JsonElement _element;
        public JsonElementAssistant(System.Text.Json.JsonElement element) => _element = element;
        public JsonElementAssistant this[string name] => new(_element.GetProperty(name));
        public string Str() => _element.GetString()!;
        public int Int() => _element.GetInt32();
        public bool Bool() => _element.GetBoolean();

        public string[] Guids() => _element.EnumerateArray().Select(v => v.GetString()!).ToArray();
    }

    /// <summary>经 entity.create 建实体并返回 guid（与 UI 同一工厂路径）。</summary>
    private string CreateEntity(string name = "Box", string kind = "Empty")
    {
        var data = Data(Invoke("entity.create", new { kind, name }));
        return data["entityGuid"].Str();
    }

    private Guid GuidOf(string s) => Guid.Parse(s);

    // ------------------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------------------

    [Fact]
    public void New_OpensSequenceWithGivenSettings()
    {
        var data = Data(Invoke("sequence.new", new { name = "开场", fps = 24, durationFrames = 48 }));

        Assert.Equal("开场", data["name"].Str());
        Assert.Equal(24, data["fps"].Int());
        Assert.Equal(48, data["durationFrames"].Int());
        Assert.True(SequenceEditorState.IsOpen);
        Assert.Null(SequenceEditorState.CurrentPath); // 未落盘
    }

    [Fact]
    public void Save_DefaultPath_IsProjectSequencesDirectory()
    {
        Invoke("sequence.new", new { name = "Intro", fps = 30, durationFrames = 24 });
        var data = Data(Invoke("sequence.save"));

        var savedPath = data["path"].Str();
        Assert.True(File.Exists(savedPath));
        Assert.Contains(Path.Combine("Sequences", "Intro.pseq"), savedPath);

        // 回读一致
        var reopened = Data(Invoke("sequence.open", new { path = savedPath }));
        Assert.Equal("Intro", reopened["name"].Str());
    }

    [Fact]
    public void Open_MissingFile_ReturnsNotFound()
    {
        Invoke("sequence.new");
        var response = Invoke("sequence.open", new { path = "Sequences/不存在.pseq" });
        Assert.False(response.Ok);
        Assert.Contains("序列", response.Error?.Message);
    }

    [Fact]
    public void Commands_WithoutOpenSequence_ReturnNotReady()
    {
        SequenceEditorState.Close();
        Assert.False(Invoke("sequence.info").Ok);
        Assert.False(Invoke("sequence.add-key", new { entityGuid = Guid.NewGuid().ToString() }).Ok);
        Assert.False(Invoke("sequence.set-frame", new { frame = 3 }).Ok);
    }

    // ------------------------------------------------------------------
    // 轨道编辑 + undo
    // ------------------------------------------------------------------

    [Fact]
    public void AddKey_AutoCreatesTrackAndKeysCurrentPose()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var guid = CreateEntity();

        // 实体摆一个非默认位置再 K 帧——省略 position 应取当前 pose
        Invoke("entity.set-transform", new { entityGuid = guid, position = new[] { 7f, 0f, 0f } });
        var data = Data(Invoke("sequence.add-key", new { entityGuid = guid, frame = 0 }));

        Assert.Equal(1, data["keys"].Int());
        var doc = SequenceEditorState.Current!;
        var track = Assert.IsType<TransformTrack>(doc.Tracks.Single());
        Assert.Equal(GuidOf(guid), track.EntityGuid);
        Assert.Equal(7f, track.Keys[0].Position[0]);
    }

    [Fact]
    public void AddKey_ExplicitPosition_WinsOverCurrentPose()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var guid = CreateEntity();
        var data = Data(Invoke("sequence.add-key", new
        {
            entityGuid = guid,
            frame = 5,
            position = new[] { 1f, 2f, 3f },
            interp = "constant",
        }));

        Assert.Equal(5, data["frame"].Int());
        var key = SequenceEditorState.Current!.Tracks.OfType<TransformTrack>().Single().Keys.Single();
        Assert.Equal(1f, key.Position[0]);
        Assert.Equal(KeyInterpolation.Constant, key.Interpolation);
    }

    [Fact]
    public void AddKey_ThenEditorUndo_RemovesKeyAndTrack()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var guid = CreateEntity();
        Invoke("sequence.add-key", new { entityGuid = guid, frame = 0, position = new[] { 1f, 1f, 1f } });
        Assert.Single(SequenceEditorState.Current!.Tracks);

        Data(Invoke("editor.undo"));
        Assert.Empty(SequenceEditorState.Current!.Tracks);
    }

    [Fact]
    public void AddKey_InvalidInterp_ReturnsInvalidParams()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var guid = CreateEntity();
        Assert.False(Invoke("sequence.add-key", new { entityGuid = guid, interp = "smooth" }).Ok);
    }

    [Fact]
    public void AddCameraCut_RequiresCameraComponent()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var plainEntity = CreateEntity();
        var err = Invoke("sequence.add-camera-cut", new
        {
            startFrame = 0,
            endFrame = 10,
            cameraEntityGuid = plainEntity,
        });
        Assert.False(err.Ok);
        Assert.Contains("Camera", err.Error?.Message);

        // 挂上 Camera 组件后再试即通过（文档模型直接加，等价 UI 添加组件）
        var entity = SequenceDriver.FindEntity(_scene.Entities, GuidOf(plainEntity))!;
        entity.Components.Add(new CameraComponent());
        Data(Invoke("sequence.add-camera-cut", new
        {
            startFrame = 0,
            endFrame = 10,
            cameraEntityGuid = plainEntity,
        }));
        Assert.Single(SequenceEditorState.Current!.CameraCut!.Shots);
    }

    [Fact]
    public void AddCameraCut_EndBeyondDuration_Clamps()
    {
        Invoke("sequence.new", new { durationFrames = 20 });
        var cam = CreateEntity("Cam");
        SequenceDriver.FindEntity(_scene.Entities, GuidOf(cam))!.Components.Add(new CameraComponent());

        Data(Invoke("sequence.add-camera-cut", new { startFrame = 0, endFrame = 999, cameraEntityGuid = cam }));
        Assert.Equal(20, SequenceEditorState.Current!.CameraCut!.Shots[0].EndFrame);
    }

    // ------------------------------------------------------------------
    // set-frame：求值 → 应用链（不经视口，纯文档模型）
    // ------------------------------------------------------------------

    [Fact]
    public void SetFrame_EvaluatesAndAppliesTransformToScene()
    {
        Invoke("sequence.new", new { durationFrames = 30 });
        var guid = CreateEntity();
        Invoke("sequence.add-key", new { entityGuid = guid, frame = 0, position = new[] { 0f, 0f, 0f } });
        Invoke("sequence.add-key", new { entityGuid = guid, frame = 10, position = new[] { 10f, 0f, 0f } });

        Data(Invoke("sequence.set-frame", new { frame = 5 }));

        var entity = SequenceDriver.FindEntity(_scene.Entities, GuidOf(guid))!;
        var transform = entity.GetTransform()!;
        Assert.Equal(5f, transform.PositionX, 5);
        Assert.Equal(5, SequenceEditorState.CurrentFrame);
    }

    [Fact]
    public void SetFrame_OutOfRange_ReturnsInvalidParams()
    {
        Invoke("sequence.new", new { durationFrames = 10 });
        Assert.False(Invoke("sequence.set-frame", new { frame = 10 }).Ok); // 有效区间 0..9
        Assert.False(Invoke("sequence.set-frame", new { frame = -1 }).Ok);
    }

    // ------------------------------------------------------------------
    // render/play：仅守卫路径（无视口/Avalonia 主窗口的单元环境）
    // ------------------------------------------------------------------

    [Fact]
    public async System.Threading.Tasks.Task Render_WithoutViewport_ReturnsNotReady()
    {
        Invoke("sequence.new", new { durationFrames = 3 });
        var registry = new CliCommandRegistry();
        registry.Register(new SequenceCommands());
        var response = await registry.InvokeAsync(Req("sequence.render", new { @out = "/tmp/seq-render-test" }));
        Assert.False(response.Ok);
        Assert.Contains("视口", response.Error?.Message);
    }
}
