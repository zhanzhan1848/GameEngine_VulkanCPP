using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Workflows;

/// <summary>步骤状态（workflows S2，2026-10-15）。</summary>
public enum StepStatus
{
    Pending = 0,
    Running = 1,
    Passed = 2,
    Failed = 3,
    Skipped = 4,   // 依赖失败 → 不执行（可解释跳过，非静默）
}

/// <summary>工作流步骤定义（依赖声明 + 幂等键 + 谓词）。</summary>
public sealed class WorkflowStepDef
{
    public string StepId { get; init; } = "";
    /// <summary>依赖的先行步骤（全部 Passed 才执行）。</summary>
    public List<string> DependsOn { get; init; } = new();
    /// <summary>外部软件/capability 依赖（缺项 → Failed，原因="capability missing"）。</summary>
    public List<string> RequiredCapabilities { get; init; } = new();
    /// <summary>幂等键：相同 (runId, key) 已 Passed → 重复提交直接复用结果不重跑。</summary>
    public string? IdempotencyKey { get; init; }
    /// <summary>昂贵步骤标记（诊断展示；不参与调度语义）。</summary>
    public bool Expensive { get; init; }
}

/// <summary>步骤运行记录。</summary>
public sealed class StepRecord
{
    public string StepId { get; set; } = "";
    public StepStatus Status { get; set; } = StepStatus.Pending;
    /// <summary>失败原因（Failed 时非空；Skipped 时=跳过原因）。</summary>
    public string? Reason { get; set; }
    /// <summary>执行尝试次数（断点恢复审计）。</summary>
    public int Attempts { get; set; }
    public DateTime? FinishedAtUtc { get; set; }
}

/// <summary>可持久化运行状态（断点=序列化此对象）。</summary>
public sealed class WorkflowRunState
{
    public const int CurrentSchemaVersion = 1;

    [JsonPropertyName("schemaVersion")] public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    [JsonPropertyName("runId")] public string RunId { get; set; } = "";
    [JsonPropertyName("steps")] public List<StepRecord> Steps { get; set; } = new();
    [JsonPropertyName("results")] public Dictionary<string, string> Results { get; set; } = new();
    [JsonPropertyName("idempotencySeen")] public Dictionary<string, string> IdempotencySeen { get; set; } = new(StringComparer.Ordinal);

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static (WorkflowRunState? state, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<WorkflowRunState>(json, JsonOptions);
            if (doc == null) return (null, "运行状态反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本——不静默误读");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"运行状态 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>步骤执行器注入（测试用假执行器/真接外部进程）。</summary>
/// <returns>(成功?, 结果字符串, 失败原因)</returns>
public delegate (bool ok, string result, string? reason) StepExecutor(WorkflowStepDef step);

/// <summary>可用 capability 集合查询（缺项 → Failed）。</summary>
public delegate bool CapabilityProbe(string capability);

/// <summary>
/// 依赖步骤执行器 + 断点恢复（workflows S2）：
///   - 拓扑序执行（无环校验，环 → 类型化错误）；
///   - 依赖失败 → 下游 Skipped（可解释，不静默）；
///   - capability 缺失 → Failed("capability missing")；
///   - 断点恢复（Resume）：**已 Passed 步骤不重跑**（昂贵步骤保护——合同原文），
///     Failed/Pending/Skipped 重评估；
///   - 幂等键：同键已 Passed → 复用结果（重复提交语义）。
/// 确定性：步骤按拓扑层 + StepId Ordinal 排序执行。
/// </summary>
public static class WorkflowRunner
{
    public sealed class RunOutcome
    {
        public bool AllPassed { get; set; }
        public int Executed;          // 本次实际执行的步骤数
        public int Reused;            // 幂等复用 / 断点跳过（已 Passed 不重跑）
        public List<string> FailedSteps { get; } = new();
        public List<string> SkippedSteps { get; } = new();
    }

    public static (WorkflowRunState? state, string? error) CreateRun(
        string runId, IReadOnlyList<WorkflowStepDef> steps)
    {
        var (order, cycleError) = TopoOrder(steps);
        if (cycleError != null) return (null, cycleError);
        var state = new WorkflowRunState { RunId = runId };
        foreach (var s in order)
            state.Steps.Add(new StepRecord { StepId = s.StepId });
        return (state, null);
    }

    /// <summary>执行/恢复一轮：返回结果摘要。state 原地更新（调用方持久化）。</summary>
    public static (RunOutcome outcome, string? error) Run(
        WorkflowRunState state,
        IReadOnlyList<WorkflowStepDef> steps,
        StepExecutor executor,
        CapabilityProbe? capabilities = null)
    {
        var (order, cycleError) = TopoOrder(steps);
        if (cycleError != null) return (new RunOutcome(), cycleError);
        var byId = steps.ToDictionary(s => s.StepId, StringComparer.Ordinal);
        var outcome = new RunOutcome();

        foreach (var step in order)
        {
            var record = state.Steps.FirstOrDefault(r => r.StepId == step.StepId);
            if (record == null) return (outcome, $"运行状态缺步骤 '{step.StepId}'（定义与状态不一致）");

            // 断点恢复：已 Passed 不重跑（昂贵步骤保护）
            if (record.Status == StepStatus.Passed)
            {
                outcome.Reused++;
                continue;
            }
            record.Attempts++;

            // 依赖检查
            var blocked = step.DependsOn
                .Select(dep => state.Steps.FirstOrDefault(r => r.StepId == dep))
                .Where(r => r == null || r.Status != StepStatus.Passed)
                .ToList();
            if (blocked.Count > 0)
            {
                var cause = blocked.First()?.Reason ?? "dependency not passed";
                record.Status = StepStatus.Skipped;
                record.Reason = $"dependency failed/skipped: {cause}";
                outcome.SkippedSteps.Add(step.StepId);
                continue;
            }

            // capability 检查
            if (capabilities != null)
            {
                var missing = step.RequiredCapabilities.FirstOrDefault(c => !capabilities(c));
                if (missing != null)
                {
                    record.Status = StepStatus.Failed;
                    record.Reason = $"capability missing: {missing}";
                    record.FinishedAtUtc = DateTime.UtcNow;
                    outcome.FailedSteps.Add(step.StepId);
                    continue;
                }
            }

            // 幂等键复用
            if (step.IdempotencyKey != null
                && state.IdempotencySeen.TryGetValue($"{step.StepId}:{step.IdempotencyKey}", out var cached))
            {
                record.Status = StepStatus.Passed;
                state.Results[step.StepId] = cached;
                record.FinishedAtUtc = DateTime.UtcNow;
                outcome.Reused++;
                continue;
            }

            record.Status = StepStatus.Running;
            var (ok, result, reason) = executor(step);
            if (ok)
            {
                record.Status = StepStatus.Passed;
                record.Reason = null;
                state.Results[step.StepId] = result;
                if (step.IdempotencyKey != null)
                    state.IdempotencySeen[$"{step.StepId}:{step.IdempotencyKey}"] = result;
            }
            else
            {
                record.Status = StepStatus.Failed;
                record.Reason = reason ?? "step failed";
                outcome.FailedSteps.Add(step.StepId);
            }
            record.FinishedAtUtc = DateTime.UtcNow;
            outcome.Executed++;
        }

        outcome.AllPassed = state.Steps.All(r => r.Status == StepStatus.Passed);
        return (outcome, null);
    }

    /// <summary>拓扑排序（层内 StepId Ordinal——确定性）；环 → 类型化错误。</summary>
    private static (List<WorkflowStepDef>, string?) TopoOrder(IReadOnlyList<WorkflowStepDef> steps)
    {
        var byId = steps.GroupBy(s => s.StepId).FirstOrDefault(g => g.Count() > 1);
        if (byId != null) return (new List<WorkflowStepDef>(), $"重复步骤 ID: {byId.Key}");

        var remaining = steps.OrderBy(s => s.StepId, StringComparer.Ordinal).ToList();
        var ordered = new List<WorkflowStepDef>();
        var done = new HashSet<string>(StringComparer.Ordinal);
        while (remaining.Count > 0)
        {
            var ready = remaining
                .Where(s => s.DependsOn.All(d => done.Contains(d)))
                .OrderBy(s => s.StepId, StringComparer.Ordinal)
                .ToList();
            if (ready.Count == 0)
                return (new List<WorkflowStepDef>(),
                    $"依赖成环或缺失: {string.Join(", ", remaining.Select(s => s.StepId))}");
            foreach (var s in ready)
            {
                ordered.Add(s);
                done.Add(s.StepId);
                remaining.Remove(s);
            }
        }
        return (ordered, null);
    }
}
