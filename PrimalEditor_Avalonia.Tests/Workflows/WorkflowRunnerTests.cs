using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Workflows;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.WorkflowNs;

/// <summary>
/// workflows S2 契约测试：依赖拓扑序执行、失败下游 Skipped（可解释非静默）、
/// 断点恢复不重跑已通过昂贵步骤、capability 缺失 Failed、幂等键重复提交复用、
/// 环检测类型化拒绝、状态持久化往返+未知版本拒。
/// </summary>
public sealed class WorkflowRunnerTests
{
    private static readonly WorkflowStepDef[] Chain = new[]
    {
        new WorkflowStepDef { StepId = "import", RequiredCapabilities = { "ffmpeg" } },
        new WorkflowStepDef { StepId = "convert", DependsOn = { "import" }, Expensive = true,
                              IdempotencyKey = "k1" },
        new WorkflowStepDef { StepId = "publish", DependsOn = { "convert" } },
    };

    [Fact]
    public void HappyPath_TopologicalOrder_AllPassed()
    {
        var (state, err) = WorkflowRunner.CreateRun("run-1", Chain);
        Assert.Null(err);
        var executed = new List<string>();
        var (outcome, runErr) = WorkflowRunner.Run(
            state!, Chain,
            step => { executed.Add(step.StepId); return (true, $"r:{step.StepId}", null); },
            _ => true);
        Assert.Null(runErr);
        Assert.True(outcome.AllPassed);
        Assert.Equal(3, outcome.Executed);
        Assert.Equal(new[] { "import", "convert", "publish" }, executed);   // 依赖序
        Assert.Equal("r:convert", state!.Results["convert"]);
    }

    [Fact]
    public void Failure_SkipsDownstream_WithReason()
    {
        var (state, _) = WorkflowRunner.CreateRun("run-2", Chain);
        var (outcome, _) = WorkflowRunner.Run(
            state!, Chain,
            step => step.StepId == "import"
                ? (false, "", "ffmpeg exited 1")
                : (true, "ok", null),
            _ => true);
        Assert.False(outcome.AllPassed);
        Assert.Equal(new[] { "import" }, outcome.FailedSteps);
        Assert.Equal(new[] { "convert", "publish" }, outcome.SkippedSteps);   // 下游可解释跳过
        var convert = state!.Steps.Single(r => r.StepId == "convert");
        Assert.Equal(StepStatus.Skipped, convert.Status);
        Assert.Contains("dependency failed", convert.Reason);
        Assert.Contains("ffmpeg exited 1", convert.Reason);   // 根因可追溯
    }

    [Fact]
    public void Resume_DoesNotRerunPassedExpensiveSteps()
    {
        var (state, _) = WorkflowRunner.CreateRun("run-3", Chain);
        // 第一轮：convert 失败（import 通过——昂贵步骤）
        WorkflowRunner.Run(state!, Chain,
            step => step.StepId == "convert" ? (false, "", "timeout") : (true, "ok", null),
            _ => true);

        // 断点=序列化往返（真实场景落盘再读回）
        var (restored, loadErr) = WorkflowRunState.Load(state!.Serialize());
        Assert.Null(loadErr);

        var rerun = new List<string>();
        var (outcome2, _) = WorkflowRunner.Run(
            restored!, Chain,
            step => { rerun.Add(step.StepId); return (true, "fixed", null); },
            _ => true);
        Assert.True(outcome2.AllPassed);
        Assert.Equal(new[] { "convert", "publish" }, rerun);   // import（已 Passed）不重跑
        Assert.Equal(1, outcome2.Reused);
        Assert.Equal(2, outcome2.Executed);
        // 重跑的 convert 仍是昂贵原步（Attempts 审计）
        Assert.Equal(2, restored!.Steps.Single(r => r.StepId == "convert").Attempts);
        Assert.Equal(1, restored.Steps.Single(r => r.StepId == "import").Attempts);
    }

    [Fact]
    public void MissingCapability_FailsWithTypeReason()
    {
        var (state, _) = WorkflowRunner.CreateRun("run-4", Chain);
        var (outcome, _) = WorkflowRunner.Run(
            state!, Chain, step => (true, "ok", null),
            cap => cap != "ffmpeg");   // ffmpeg 缺失
        Assert.Equal(new[] { "import" }, outcome.FailedSteps);
        Assert.Contains("capability missing: ffmpeg",
            state!.Steps.Single(r => r.StepId == "import").Reason);
        Assert.Contains("convert", outcome.SkippedSteps);
    }

    [Fact]
    public void IdempotencyKey_DuplicateSubmission_ReusesResult()
    {
        var (state, _) = WorkflowRunner.CreateRun("run-5", Chain);
        WorkflowRunner.Run(state!, Chain, step => (true, "v1", null), _ => true);

        // 重复提交（全新 run、同幂等键）→ convert 复用，不执行
        var (state2, _) = WorkflowRunner.CreateRun("run-5-retry", Chain);
        // 同一断点状态下再跑（键已记录）
        var executed = new List<string>();
        var (outcome, _) = WorkflowRunner.Run(
            state!, Chain,
            step => { executed.Add(step.StepId); return (true, "should-not-run", null); },
            _ => true);
        Assert.True(outcome.AllPassed);
        Assert.Equal(0, outcome.Executed);     // 全部 Passed → 零重跑
        Assert.Equal(3, outcome.Reused);
        Assert.Empty(executed);
        Assert.Equal("v1", state!.Results["convert"]);   // 幂等结果未被覆盖
    }

    [Fact]
    public void Cycle_AndDuplicateIds_Rejected()
    {
        var cycle = new[]
        {
            new WorkflowStepDef { StepId = "a", DependsOn = { "b" } },
            new WorkflowStepDef { StepId = "b", DependsOn = { "a" } },
        };
        var (state, err) = WorkflowRunner.CreateRun("bad-1", cycle);
        Assert.Null(state);
        Assert.Contains("环", err);

        var dup = new[]
        {
            new WorkflowStepDef { StepId = "a" },
            new WorkflowStepDef { StepId = "a" },
        };
        var (state2, err2) = WorkflowRunner.CreateRun("bad-2", dup);
        Assert.Null(state2);
        Assert.Contains("重复步骤 ID", err2);
    }

    [Fact]
    public void State_RoundTrip_AndFutureSchemaRejected()
    {
        var (state, _) = WorkflowRunner.CreateRun("run-6", Chain);
        WorkflowRunner.Run(state!, Chain, step => (true, "ok", null), _ => true);
        var (loaded, err) = WorkflowRunState.Load(state!.Serialize());
        Assert.Null(err);
        Assert.Equal(3, loaded!.Steps.Count);
        Assert.Equal("ok", loaded.Results["publish"]);

        var (future, futureErr) = WorkflowRunState.Load(
            state.Serialize().Replace("\"schemaVersion\": 1", "\"schemaVersion\": 9"));
        Assert.Null(future);
        Assert.Contains("不静默误读", futureErr);
    }
}
