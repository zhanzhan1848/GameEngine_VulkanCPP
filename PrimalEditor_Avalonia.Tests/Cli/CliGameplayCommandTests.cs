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
/// gameplay.* 命令面的三道门语义（S2）：提案门（staged 不生效）、确认门
///（apply 原子生效 + 前像/备份）、回滚（跨会话前像恢复）。dry-run 依赖
/// PrimalPlayer 进程（窗口式），单测只覆盖守卫路径（帧数边界/状态机拒绝），
/// 行为证据由 EngineTest/GameplayTests 的 E2E 与 Player dryrun 冒烟承担。
/// </summary>
public sealed class CliGameplayCommandTests : IDisposable
{
    private readonly Project _project;
    private readonly Scene _scene;
    private readonly WorldEditorViewModel _worldEditor;

    public CliGameplayCommandTests()
    {
        _project = Project.CreateNew("CliGpTest",
            Path.Combine(Path.GetTempPath(), "cli-gp-test-" + Guid.NewGuid().ToString("N")));
        _scene = new Scene("Main");
        var door = new GameEntity("Door");
        door.Components.Add(new Transform());
        door.Components.Add(new GameplayRulesComponent
        {
            InitialState = "closed",
            States = { "closed", "open" },
        });
        _scene.Entities.Add(door);
        _project.Scenes.Add(_scene);
        _worldEditor = new WorldEditorViewModel { Project = _project, ActiveScene = _scene };
        EditorServices.WorldEditorOverride = _worldEditor;

        Directory.CreateDirectory(Path.Combine(_project.Path, "Content", "Scripts"));
        File.WriteAllText(Path.Combine(_project.Path, "Content", "Scripts", "door.lua"),
            "-- baseline\nreturn {}\n");
    }

    public void Dispose()
    {
        EditorServices.WorldEditorOverride = null;
        try { Directory.Delete(_project.Path, recursive: true); } catch { }
    }

    private static CliResponse Invoke(string verb, object? parameters = null)
    {
        var registry = new CliCommandRegistry();
        registry.Register(new GameplayCommands());
        return registry.InvokeAsync(new CliRequest
        {
            Id = "t",
            Verb = verb,
            Params = System.Text.Json.JsonSerializer.SerializeToElement(parameters ?? new { }),
        }).GetAwaiter().GetResult();
    }

    private static JsonElementAssistant Data(CliResponse r)
    {
        Assert.True(r.Ok, $"expected ok, got {r.Error?.Code}: {r.Error?.Message}");
        return new JsonElementAssistant(r.Data!.Value);
    }

    /// <summary>JsonElement 的轻量取值助手（同 CliEntityCommandTests 惯例）。</summary>
    private readonly struct JsonElementAssistant
    {
        private readonly System.Text.Json.JsonElement _element;
        public JsonElementAssistant(System.Text.Json.JsonElement element) => _element = element;
        public JsonElementAssistant this[string name] => new(_element.GetProperty(name));
        public JsonElementAssistant this[int index] => new(_element[index]);
        public string Str() => _element.GetString()!;
        public string String(string name) => _element.GetProperty(name).GetString()!;
        public bool Bool(string name) => _element.GetProperty(name).GetBoolean();
        public int Int(string name) => _element.GetProperty(name).GetInt32();
        public int ArrLen(string name) => _element.GetProperty(name).GetArrayLength();
        public JsonElementAssistant At(string path)
        {
            var cur = _element;
            foreach (var seg in path.Split('.'))
                cur = int.TryParse(seg, out var i) ? cur[i] : cur.GetProperty(seg);
            return new JsonElementAssistant(cur);
        }
    }

    private string ProjRel(string rel) => Path.Combine(_project.Path, rel);

    [Fact]
    public void Propose_StagesFilesWithoutTakingEffect()
    {
        var proposed = Path.Combine(_project.Path, "Content", "Scripts", "door_v2.lua");
        File.WriteAllText(proposed, "-- v2\nreturn {}\n");

        var d = Data(Invoke("gameplay.propose", new
        {
            name = "door-faster",
            filesCsv = "Content/Scripts/door_v2.lua",
            note = "second iteration",
        }));
        Assert.Equal("proposed", d.String("status"));
        Assert.False(d.Bool("effective"));
        Assert.True(File.Exists(Path.Combine(_project.Path,
            ".Primal/gameplay/proposals/door-faster/files/Content/Scripts/door_v2.lua")));

        // 提案不生效：项目内原文件保持不动
        Assert.DoesNotContain("v2", File.ReadAllText(ProjRel("Content/Scripts/door.lua")));
    }

    [Fact]
    public void Propose_RejectsNonLuaAndMissingFiles()
    {
        Assert.False(Invoke("gameplay.propose", new { name = "bad", filesCsv = "Content/Scripts/x.txt" }).Ok);
        Assert.False(Invoke("gameplay.propose", new { name = "bad", filesCsv = "Content/Scripts/nope.lua" }).Ok);
    }

    [Fact]
    public void Apply_SwapsFilesAndPatchesRules_ThenRollbackRestores()
    {
        var variant = Path.Combine(Path.GetTempPath(), "gp_variant_" + Guid.NewGuid().ToString("N") + ".lua");
        File.WriteAllText(variant, "-- v2 open faster\nreturn {}\n");
        var patchJson = """[{"entity":"Door","rules":{"InitialState":"closed","States":["closed","open","ajar"],"Transitions":[]}}]""";

        Data(Invoke("gameplay.propose", new
        {
            name = "p1",
            filesCsv = variant + "|Content/Scripts/door.lua",
            rulesPatchJson = patchJson,
        }));
        var applied = Data(Invoke("gameplay.apply", new { name = "p1" }));
        Assert.True(applied.Bool("applied"));
        Assert.Equal(1, applied.Int("rulesPatched"));

        // 文件已切换
        Assert.Contains("v2", File.ReadAllText(ProjRel("Content/Scripts/door.lua")));
        // 规则 patch 已生效（文档模型）
        var rules = _scene.Entities.Single().GetComponent<GameplayRulesComponent>()!;
        Assert.Equal(3, rules.States.Count);
        Assert.Contains("ajar", rules.States);
        // Undo 也可用（确认门走文档模型）
        _worldEditor.UndoRedo.Undo();
        Assert.Equal(2, _scene.Entities.Single().GetComponent<GameplayRulesComponent>()!.States.Count);
        _worldEditor.UndoRedo.Redo();
        Assert.Equal(3, _scene.Entities.Single().GetComponent<GameplayRulesComponent>()!.States.Count);

        // 状态门：applied 再 apply 拒绝
        Assert.False(Invoke("gameplay.apply", new { name = "p1" }).Ok);

        // 回滚：文件恢复 + 前像恢复
        var rb = Data(Invoke("gameplay.rollback", new { name = "p1" }));
        Assert.True(rb.Bool("rolledBack"));
        Assert.DoesNotContain("v2", File.ReadAllText(ProjRel("Content/Scripts/door.lua")));
        Assert.Equal(2, _scene.Entities.Single().GetComponent<GameplayRulesComponent>()!.States.Count);

        // 回滚后可重新 apply（状态回 proposed）
        var again = Data(Invoke("gameplay.apply", new { name = "p1" }));
        Assert.True(again.Bool("applied"));
    }

    [Fact]
    public void Apply_RejectsPatchTargetMissing()
    {
        File.WriteAllText(Path.Combine(_project.Path, "Content", "Scripts", "d.lua"), "return {}\n");
        Data(Invoke("gameplay.propose", new
        {
            name = "p2",
            filesCsv = "Content/Scripts/d.lua",
            rulesPatchJson = """[{"entity":"Ghost","rules":{"InitialState":"a","States":["a"]}}]""",
        }));
        var r = Invoke("gameplay.apply", new { name = "p2" });
        Assert.False(r.Ok);
        // 原子性：patch 失败不影响已 apply 的文件？——apply 顺序=文件先换。
        // 契约：patch 目标缺失属提案数据错误，文件已备份可 rollback。
        Assert.Contains("Ghost", r.Error!.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void DryRun_GuardsFramesAndStatus()
    {
        File.WriteAllText(Path.Combine(_project.Path, "Content", "Scripts", "d.lua"), "return {}\n");
        Data(Invoke("gameplay.propose", new { name = "p3", filesCsv = "Content/Scripts/d.lua" }));
        Assert.False(Invoke("gameplay.dryrun", new { name = "p3", frames = 0 }).Ok);
        Assert.False(Invoke("gameplay.dryrun", new { name = "p3", timeoutSec = 1 }).Ok);
        Assert.False(Invoke("gameplay.dryrun", new { name = "ghost" }).Ok);
    }

    [Fact]
    public void Status_ListsProposals()
    {
        File.WriteAllText(Path.Combine(_project.Path, "Content", "Scripts", "d.lua"), "return {}\n");
        Data(Invoke("gameplay.propose", new { name = "s1", filesCsv = "Content/Scripts/d.lua" }));
        var d = Data(Invoke("gameplay.status"));
        Assert.Equal("s1", d.At("proposals.0").String("name"));
        Assert.Equal("proposed", d.At("proposals.0").String("status"));
    }
}
