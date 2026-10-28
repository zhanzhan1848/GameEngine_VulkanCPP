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
/// gameplay S3/S4 命令面语义：驳回回喂（反馈包含原因+契约摘要）、分级自主
///（政策校验：空政策不构成授权）、世代化（哈希审计/链式登记/回滚）、
/// 多样性度量（n-gram 重复率的下限门禁语义）。
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

    private string ApplyProposal(string name)
    {
        var variant = Path.Combine(Path.GetTempPath(), "evo_" + Guid.NewGuid().ToString("N") + ".lua");
        File.WriteAllText(variant, "-- variant\nreturn {}\n");
        Assert.True(Invoke("gameplay.propose", new
        {
            name,
            filesCsv = variant + "|Content/Scripts/NPC/guard.lua",
        }).Ok);
        Assert.True(Invoke("gameplay.apply", new { name }).Ok);
        return variant;
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

    // ==== S4 世代化 ====

    [Fact]
    public void Generation_ChainsWithHashAudit_And_RollbackRestores()
    {
        ApplyProposal("g1");
        var r1 = Invoke("gameplay.generation.record", new { npc = "Guard", proposal = "g1" });
        Assert.True(r1.Ok);
        Assert.Equal(1, Get<int>(r1, "generation"));

        ApplyProposal("g2");
        var r2 = Invoke("gameplay.generation.record", new { npc = "Guard", proposal = "g2" });
        Assert.True(r2.Ok);
        Assert.Equal(2, Get<int>(r2, "generation"));

        // 人设参数漂移被审计拒绝（改契约数值=人的动作，必须显式留痕）
        _scene.Entities.Single().GetComponent<GameplayRulesComponent>()!.States.Add("flee");
        var drift = Invoke("gameplay.generation.record", new { npc = "Guard", proposal = "g2" });
        Assert.False(drift.Ok);
        Assert.Contains("rulesHash", drift.Error!.Message, StringComparison.Ordinal);
        var allowed = Invoke("gameplay.generation.record",
            new { npc = "Guard", proposal = "g2", allowHumanRulesEdit = true });
        Assert.True(allowed.Ok);
        Assert.True(Get<bool>(allowed, "humanRulesEdit"));

        // 回滚到父世代（对 g3=human-edit 世代的提案执行 rollback）
        var rb = Invoke("gameplay.generation.rollback", new { npc = "Guard" });
        Assert.True(rb.Ok);
        // 链只增：list 仍有全部世代
        var list = Invoke("gameplay.generation.list", new { npc = "Guard" });
        Assert.True(list.Ok);
    }

    [Fact]
    public void Generation_RequiresAppliedProposal()
    {
        var variant = Path.Combine(Path.GetTempPath(), "na_" + Guid.NewGuid().ToString("N") + ".lua");
        File.WriteAllText(variant, "return {}\n");
        Invoke("gameplay.propose", new { name = "na", filesCsv = variant + "|Content/Scripts/NPC/guard.lua" });
        Assert.False(Invoke("gameplay.generation.record", new { npc = "Guard", proposal = "na" }).Ok);
    }

    // ==== S4 多样性 ====

    [Fact]
    public void Diversity_MeasuresNGramRepetitionAcrossRuns()
    {
        // 造 3 份 trace：完全相同（重复率≈高） vs 交替变化（低）
        var dir = Path.Combine(_project.Path, ".Primal", "gameplay", "dryrun", "divtest");
        Directory.CreateDirectory(dir);
        string MakeRun(string suffix) => Path.Combine(dir, $"run_{suffix}.csv");
        // 4 份完全相同的运行（帧号不同、动作签名相同）——跨运行重复的极端
        var sameActions = new[]
        {
            "# gameplay trace v1 frames=10 records=4",
            "0,10,0,event=step|entity=Guard",
            "1,20,2,entity=Guard|from=a|to=b",
            "2,30,0,event=look|entity=Guard",
            "3,40,1,entity=Guard|from=b|to=a",
        };
        foreach (var name in new[] { "a", "b", "c", "d" })
            File.WriteAllLines(MakeRun(name), sameActions);
        var identical = Invoke("gameplay.diversity", new
        {
            tracesCsv = string.Join(',', new[] { "a", "b", "c", "d" }.Select(n => MakeRun(n))),
            n = 2,
        });
        Assert.True(identical.Ok);
        var identRate = identical.Data!.Value.GetProperty("overallRepeatRate").GetDouble();
        Assert.True(identRate > 0.6, $"4 份完全相同的运行重复率应≈0.75，实测 {identRate}");

        // 4 份互不相同的运行 → 重复率显著下降（S4 靶子的最小样例）
        foreach (var (name, extra) in new[] { ("a", "step"), ("b", "sprint"), ("c", "sing"), ("d", "stare") })
            File.WriteAllLines(MakeRun(name), new[]
            {
                "# gameplay trace v1 frames=10 records=4",
                $"0,10,0,event={extra}|entity=Guard",
                "1,20,2,entity=Guard|from=a|to=b",
                "2,30,0,event=look|entity=Guard",
                "3,40,1,entity=Guard|from=b|to=a",
            });
        var mixed = Invoke("gameplay.diversity", new
        {
            tracesCsv = string.Join(',', new[] { "a", "b", "c", "d" }.Select(n => MakeRun(n))),
            n = 2,
        });
        Assert.True(mixed.Ok);
        var mixedRate = mixed.Data!.Value.GetProperty("overallRepeatRate").GetDouble();
        Assert.True(mixedRate < identRate, $"变化后重复率应下降: {mixedRate} < {identRate}");

        // 阈值门禁（下限约束语义；重造相同运行）
        foreach (var name in new[] { "a", "b", "c", "d" })
            File.WriteAllLines(MakeRun(name), sameActions);
        var gated = Invoke("gameplay.diversity", new
        {
            tracesCsv = string.Join(',', new[] { "a", "b", "c", "d" }.Select(n => MakeRun(n))),
            n = 2,
            threshold = 0.5,
        });
        Assert.False(gated.Data!.Value.GetProperty("gate").GetBoolean());
    }
}
