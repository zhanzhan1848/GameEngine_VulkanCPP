using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// gameplay S3（gameplay-rules.md）：共创回路的驳回回喂与分级自主。
/// NPC 世代化与多样性门禁。
///
/// 归属纪律：引擎零改动（全部复用 S1/S2 的 trace/dryrun/提案机制）；
/// LLM 调用只发生在外层 harness（PrimalCli/MCP 侧）——本文件提供它需要的
/// 观测面。
///
///   S3 驳回回喂  gameplay.reject      —— 驳回原因记入 manifest，返回反馈包
///                                    （原因+最近证据+当前契约摘要），AI 迭代
///                                    不丢上下文；
///   S3 分级自主  gameplay.autonomy.*  —— 人批准的政策（沙盒预算/路径白名单/
///                                    每会话自动生效上限）；manual=每提案必审。
///   S4 世代化    gameplay.generation.* —— 世代登记（proposal+记忆快照+证据+
///                                    人设参数哈希审计）/列表/回滚（世代切换点
///                                    原子回退）；
///   S4 多样性    gameplay.diversity   —— 行为流 n-gram 重复率（只作下限约束：
///                                    重复率须低于阈值，不追求最小化）。
/// </summary>
public sealed class GameplayEvolutionCommands
{
    // ==== 自治政策（.Primal/gameplay/autonomy.json；人持有）====
    public sealed class AutonomyPolicy
    {
        // manual：每提案必审（初期默认）。sandbox：审政策不审个案——回路在
        // 预算与白名单内可自主生效，但生成级切换（generation rollback）恒需人。
        [JsonPropertyName("mode")] public string Mode { get; set; } = "manual";
        [JsonPropertyName("maxAutoAppliesPerSession")] public int MaxAutoAppliesPerSession { get; set; } = 0;
        [JsonPropertyName("maxFramesPerDryrun")] public int MaxFramesPerDryrun { get; set; } = 600;
        [JsonPropertyName("timeoutSecPerDryrun")] public int TimeoutSecPerDryrun { get; set; } = 180;
        // 行为文件白名单（项目相对前缀）；空 = 沙盒模式拒绝一切自动生效。
        [JsonPropertyName("allowedPathPrefixes")] public List<string> AllowedPathPrefixes { get; set; } = new();
        [JsonPropertyName("updatedAtUtc")] public DateTime UpdatedAtUtc { get; set; }
    }

    private static string AutonomyPath(string projectDir) =>
        Path.Combine(projectDir, ".Primal", "gameplay", "autonomy.json");

    private static AutonomyPolicy LoadAutonomy(string projectDir)
    {
        var p = AutonomyPath(projectDir);
        if (!File.Exists(p)) return new AutonomyPolicy { UpdatedAtUtc = DateTime.UtcNow };
        return JsonSerializer.Deserialize<AutonomyPolicy>(File.ReadAllText(p),
                   new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
               ?? new AutonomyPolicy { UpdatedAtUtc = DateTime.UtcNow };
    }

    private static void SaveAutonomy(string projectDir, AutonomyPolicy policy)
    {
        var p = AutonomyPath(projectDir);
        Directory.CreateDirectory(Path.GetDirectoryName(p)!);
        policy.UpdatedAtUtc = DateTime.UtcNow;
        File.WriteAllText(p, JsonSerializer.Serialize(policy,
            new JsonSerializerOptions { WriteIndented = true }));
    }

    private static (WorldEditorViewModel editor, string projectDir) RequireProject()
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        if (worldEditor.ActiveScene == null)
            throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");
        var projectDir = worldEditor.Project?.Path
            ?? throw CliError.NotReady("项目未加载", "先在编辑器中打开项目");
        return (worldEditor, projectDir);
    }

    // ==== S3：驳回回喂 ====

    [CliCommand("gameplay.reject",
        Description = "驳回提案并把原因回喂给 AI 迭代（人的决定面之一，与 apply/只调参数 并列——" +
                  "单次确认 ≤3 个决定）。reason 必填（回喂的上下文核心）；manifest 记 rejected + " +
                  "reason + 时间，状态回可重提（同名 re-propose 即迭代，历史 verdict 保留）。" +
                  "返回反馈包：原因 + 最近 verdict 指针 + 当前契约摘要（实体→规则组件 JSON），" +
                  "AI 拿它迭代不丢上下文。失败：NOT_FOUND（提案不存在）、INVALID_PARAMS" +
                  "（reason 为空）。",
        Example = """{"name":"door-faster","reason":"开门太快，破坏探索节奏；保留触发距离"}""")]
    public object Reject(string name, string reason)
    {
        if (string.IsNullOrWhiteSpace(reason))
            throw CliError.InvalidParams("reason 必填", "驳回原因回喂 AI 迭代——是回路的核心上下文");
        var (worldEditor, projectDir) = RequireProject();
        var dir = Path.Combine(projectDir, ".Primal", "gameplay", "proposals", name);
        var mfPath = Path.Combine(dir, "proposal.json");
        if (!File.Exists(mfPath))
            throw CliError.NotFound($"提案 '{name}'", "先 gameplay.propose");

        var manifest = JsonSerializer.Deserialize<GameplayCommands.ProposalManifest>(
                File.ReadAllText(mfPath), new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw CliError.InvalidParams("manifest 损坏", "重新 propose");
        if (manifest.Status == "applied")
            throw CliError.InvalidParams($"提案 '{name}' 已 applied", "先 gameplay.rollback 再驳回");

        var rejectionsPath = Path.Combine(dir, "rejections.jsonl");
        File.AppendAllText(rejectionsPath, JsonSerializer.Serialize(new
        {
            atUtc = DateTime.UtcNow,
            reason,
        }) + "\n");
        manifest.Status = "rejected";
        var manifestType = manifest.GetType();
        File.WriteAllText(mfPath, JsonSerializer.Serialize(manifest,
            new JsonSerializerOptions { WriteIndented = true }));

        // 反馈包：当前契约摘要（人持有的 L0 数据——AI 只读）
        var contract = worldEditor.ActiveScene!.Entities
            .Where(e => e.GetComponent<GameplayRulesComponent>() != null)
            .Select(e => new
            {
                entity = e.Name,
                rules = JsonSerializer.SerializeToElement(
                    e.GetComponent<GameplayRulesComponent>(), SceneDocument.JsonOptions),
            })
            .ToList();
        return new
        {
            rejected = true,
            proposal = name,
            feedback = new
            {
                reason,
                lastVerdict = manifest.LastVerdict,
                contract,
                iterateHint = $"同名 re-propose 即迭代（gameplay.propose {name} ...）；历史证据未删",
            },
            decisionSurface = new[] { "approve=gameplay.apply", "reject=本命令", "tune=人改 L0 数值（UI/组件命令+Undo，无需 AI 重写）" },
        };
    }

    // ==== S3：分级自主 ====

    [CliCommand("gameplay.autonomy.get",
        Description = "读自治政策（只读）。manual=每提案必审（默认）；sandbox=审政策不审个案" +
                  "（回路在 maxFramesPerDryrun/timeoutSecPerDryrun/allowedPathPrefixes 预算内" +
                  "可自主 dryrun+apply，受 maxAutoAppliesPerSession 上限约束；世代级回滚恒需人）。",
        Example = "{}")]
    public object AutonomyGet()
    {
        var (_, projectDir) = RequireProject();
        return LoadAutonomy(projectDir);
    }

    [CliCommand("gameplay.autonomy.set",
        Description = "写自治政策（人批准预算与约束=政策审的核心动作）。mode=manual|sandbox；" +
                  "sandbox 必须显式给 allowedPathPrefixes（逗号分隔项目相对前缀，如 " +
                  "Content/Scripts/NPC）与 maxAutoAppliesPerSession>0，否则 INVALID_PARAMS" +
                  "（空政策不构成授权）。预算字段同 get 的说明。",
        Example = """{"mode":"sandbox","allowedPathPrefixesCsv":"Content/Scripts/NPC","maxAutoAppliesPerSession":5}""")]
    public object AutonomySet(string mode, string? allowedPathPrefixesCsv = null,
        int maxAutoAppliesPerSession = 0, int maxFramesPerDryrun = 600, int timeoutSecPerDryrun = 180)
    {
        var (_, projectDir) = RequireProject();
        var policy = LoadAutonomy(projectDir);
        if (!string.Equals(mode, "manual", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(mode, "sandbox", StringComparison.OrdinalIgnoreCase))
            throw CliError.InvalidParams($"未知 mode '{mode}'", "manual / sandbox");
        var isSandbox = string.Equals(mode, "sandbox", StringComparison.OrdinalIgnoreCase);
        var prefixes = (allowedPathPrefixesCsv ?? "")
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(p => p.Replace('\\', '/')).ToList();
        if (isSandbox)
        {
            if (prefixes.Count == 0)
                throw CliError.InvalidParams("sandbox 模式必须给 allowedPathPrefixes",
                    "例：Content/Scripts/NPC（空政策不构成授权）");
            if (maxAutoAppliesPerSession < 1)
                throw CliError.InvalidParams("sandbox 模式 maxAutoAppliesPerSession 必须 ≥1");
        }
        if (maxFramesPerDryrun < 10 || maxFramesPerDryrun > 100_000 ||
            timeoutSecPerDryrun < 5 || timeoutSecPerDryrun > 3600)
            throw CliError.InvalidParams("dryrun 预算越界", "frames 10..100000, timeoutSec 5..3600");
        policy.Mode = isSandbox ? "sandbox" : "manual";
        policy.MaxAutoAppliesPerSession = isSandbox ? maxAutoAppliesPerSession : 0;
        policy.AllowedPathPrefixes = isSandbox ? prefixes : new List<string>();
        policy.MaxFramesPerDryrun = maxFramesPerDryrun;
        policy.TimeoutSecPerDryrun = timeoutSecPerDryrun;
        SaveAutonomy(projectDir, policy);
        return new { policy, note = "世代级回滚（gameplay.generation.rollback）不受 sandbox 授权影响——恒需人" };
    }

}
