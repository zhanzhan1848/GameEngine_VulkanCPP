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
/// gameplay S3 命令面语义：驳回回喂（反馈包含原因+契约摘要）、分级自主
///（政策校验：空政策不构成授权）。
/// </summary>
public sealed class CliGameplayEvolutionTests : IDisposable
{
    private readonly Project _project;
    private readonly Scene _scene;
    private readonly WorldEditorViewModel _worldEditor;

    public CliGameplayEvolutionTests()
    {
        _project = Project.CreateNew("CliGpEvoTest",
            Path.Combine(Path.GetTempPath(), "cli-gp-evo-" + Guid.NewGuid().ToString("N")));
        _scene = new Scene("Main");
        var guard = new GameEntity("Guard");
        guard.Components.Add(new Transform());
        guard.Components.Add(new GameplayRulesComponent
        {
            InitialState = "patrol",
            States = { "patrol", "alert" },
        });
        _scene.Entities.Add(guard);
        _project.Scenes.Add(_scene);
        _worldEditor = new WorldEditorViewModel { Project = _project, ActiveScene = _scene };
        EditorServices.WorldEditorOverride = _worldEditor;

        Directory.CreateDirectory(Path.Combine(_project.Path, "Content", "Scripts", "NPC"));
        File.WriteAllText(Path.Combine(_project.Path, "Content", "Scripts", "NPC", "guard.lua"),
            "-- baseline\nreturn {}\n");
    }

    public void Dispose()
    {
        EditorServices.WorldEditorOverride = null;
        try { Directory.Delete(_project.Path, recursive: true); } catch { }
    }

    private CliResponse Invoke(string verb, object? parameters = null)
    {
        var registry = new CliCommandRegistry();
        registry.Register(new GameplayCommands());
        registry.Register(new GameplayEvolutionCommands());
        return registry.InvokeAsync(new CliRequest
        {
            Id = "t",
            Verb = verb,
            Params = System.Text.Json.JsonSerializer.SerializeToElement(parameters ?? new { }),
        }).GetAwaiter().GetResult();
    }

    private T Get<T>(CliResponse r, string prop)
    {
        Assert.True(r.Ok, $"expected ok, got {r.Error?.Code}: {r.Error?.Message}");
        return System.Text.Json.JsonSerializer.Deserialize<T>(r.Data!.Value.GetProperty(prop))!;
    }

    // ==== S3 驳回回喂 ====

    [Fact]
    public void Reject_RecordsReasonAndReturnsFeedbackWithContract()
    {
        var variant = Path.Combine(Path.GetTempPath(), "rej_" + Guid.NewGuid().ToString("N") + ".lua");
        File.WriteAllText(variant, "-- v\nreturn {}\n");
        Assert.True(Invoke("gameplay.propose", new
        {
            name = "r1",
            filesCsv = variant + "|Content/Scripts/NPC/guard.lua",
        }).Ok);

        var fb = Invoke("gameplay.reject", new { name = "r1", reason = "巡逻节奏太机械" });
        Assert.True(fb.Ok);
        var feedback = fb.Data!.Value.GetProperty("feedback");
        Assert.Equal("巡逻节奏太机械", feedback.GetProperty("reason").GetString());
        // 反馈包含当前契约摘要（人持有的 L0 数据只读回喂）
        var contract = feedback.GetProperty("contract");
        Assert.Equal(1, contract.GetArrayLength());
        Assert.Equal("Guard", contract[0].GetProperty("entity").GetString());
        // 决定面 ≤3
        var surface = fb.Data.Value.GetProperty("decisionSurface");
        Assert.True(surface.GetArrayLength() <= 3);

        // 驳回后状态拒绝 apply
        Assert.False(Invoke("gameplay.apply", new { name = "r1" }).Ok);
        // reason 必填
        Assert.False(Invoke("gameplay.reject", new { name = "r1" }).Ok);
    }

    // ==== S3 分级自主 ====

    [Fact]
    public void Autonomy_SandboxRequiresPolicyGrant()
    {
        // 空政策不构成授权
        Assert.False(Invoke("gameplay.autonomy.set", new { mode = "sandbox" }).Ok);
        Assert.False(Invoke("gameplay.autonomy.set",
            new { mode = "sandbox", allowedPathPrefixesCsv = "Content/Scripts/NPC" }).Ok);
        var ok = Invoke("gameplay.autonomy.set", new
        {
            mode = "sandbox",
            allowedPathPrefixesCsv = "Content/Scripts/NPC",
            maxAutoAppliesPerSession = 3,
        });
        Assert.True(ok.Ok);
        Assert.Equal("sandbox", ok.Data!.Value.GetProperty("policy").GetProperty("mode").GetString());
        // manual 回退清空授权
        var back = Invoke("gameplay.autonomy.set", new { mode = "manual" });
        Assert.True(back.Ok);
        var pol = Invoke("gameplay.autonomy.get");
        Assert.True(pol.Ok);
        Assert.Equal("manual", pol.Data!.Value.GetProperty("mode").GetString());
        Assert.Equal(0, pol.Data.Value.GetProperty("maxAutoAppliesPerSession").GetInt32());
    }

}
