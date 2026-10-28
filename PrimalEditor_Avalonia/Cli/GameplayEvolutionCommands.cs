using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// gameplay S3/S4（gameplay-rules.md）：共创回路的分级自主、驳回回喂、
/// NPC 世代化与多样性门禁。
///
/// 归属纪律：引擎零改动（全部复用 S1/S2 的 trace/dryrun/提案机制）；
/// LLM 调用只发生在外层 harness（PrimalCli/MCP 侧）——本文件提供它需要的
/// 观测/登记/回滚/度量面。
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

    // ==== S4：世代化 ====

    public sealed class GenerationEntry
    {
        [JsonPropertyName("npc")] public string Npc { get; set; } = "";
        [JsonPropertyName("generation")] public int Generation { get; set; }
        [JsonPropertyName("parent")] public int? Parent { get; set; }
        [JsonPropertyName("proposal")] public string Proposal { get; set; } = "";
        [JsonPropertyName("createdAtUtc")] public DateTime CreatedAtUtc { get; set; }
        // 人设参数（L0 契约）规范化哈希——世代间必须零改动（审计断言）。
        [JsonPropertyName("rulesHash")] public string RulesHash { get; set; } = "";
        // 行为证据指针（dryrun 工作目录相对项目的路径）。
        [JsonPropertyName("evidence")] public string? Evidence { get; set; }
        // 记忆快照（事件流投影文件相对项目的路径）。
        [JsonPropertyName("memory")] public string? Memory { get; set; }
        [JsonPropertyName("note")] public string? Note { get; set; }
    }

    private static string GenerationsRoot(string projectDir) =>
        Path.Combine(projectDir, ".Primal", "gameplay", "generations");

    /// <summary>当前激活场景全部 GameplayRulesComponent 的规范化哈希（人设参数审计）。</summary>
    internal static string ComputeRulesHash(Scene scene)
    {
        var sb = new StringBuilder();
        foreach (var e in scene.Entities.OrderBy(e => e.Name, StringComparer.Ordinal))
        {
            var rules = e.GetComponent<GameplayRulesComponent>();
            if (rules == null) continue;
            sb.Append(e.Name).Append('\n');
            sb.Append(JsonSerializer.Serialize(rules, SceneDocument.JsonOptions)).Append('\n');
        }
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    [CliCommand("gameplay.generation.record",
        Description = "登记一个 NPC 世代切换点（generation id 单调 +1；改进只发生在切换点）。" +
                  "proposal 为已 applied 的提案名（证据链）；memoryCsv 可选=该 NPC 的 trace " +
                  "CSV（事件流投影，存为世代记忆快照）。审计断言：rulesHash（当前场景全部规则" +
                  "组件的规范化哈希）与父世代必须一致——人设参数世代间零改动，人是缰绳；" +
                  "不一致直接 INVALID_PARAMS（除非 allowHumanRulesEdit=true 显式标注这是人的" +
                  "修改——仍会记录变更并留痕）。失败：NOT_FOUND（提案未 applied）、" +
                  "INVALID_PARAMS（哈希漂移/参数非法）。",
        Example = """{"npc":"Guard","proposal":"guard-g3","memoryCsv":".Primal/gameplay/dryrun/guard-g3-x/trace.csv"}""")]
    public object GenerationRecord(string npc, string proposal, string? memoryCsv = null,
        string? note = null, bool allowHumanRulesEdit = false)
    {
        var (worldEditor, projectDir) = RequireProject();
        if (string.IsNullOrWhiteSpace(npc)) throw CliError.InvalidParams("npc 必填");
        var proposalsRoot = Path.Combine(projectDir, ".Primal", "gameplay", "proposals");
        var mfPath = Path.Combine(proposalsRoot, proposal, "proposal.json");
        if (!File.Exists(mfPath))
            throw CliError.NotFound($"提案 '{proposal}'", "世代必须指向已存在提案");
        var manifest = JsonSerializer.Deserialize<GameplayCommands.ProposalManifest>(
                File.ReadAllText(mfPath), new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw CliError.InvalidParams("提案 manifest 损坏");
        if (manifest.Status != "applied")
            throw CliError.InvalidParams($"提案 '{proposal}' 未 applied（status={manifest.Status}）",
                "世代登记在确认门之后——先 gameplay.apply");

        var root = Path.Combine(GenerationsRoot(projectDir), npc);
        Directory.CreateDirectory(root);
        var generations = Directory.GetFiles(root, "gen-*.json")
            .Select(f => JsonSerializer.Deserialize<GenerationEntry>(File.ReadAllText(f),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true }))
            .Where(g => g != null).Select(g => g!).OrderBy(g => g.Generation).ToList();
        var parent = generations.Count > 0 ? generations[^1].Generation : (int?)null;

        var rulesHash = ComputeRulesHash(worldEditor.ActiveScene!);
        var hashDrift = parent != null && generations[^1].RulesHash != rulesHash;
        if (hashDrift && !allowHumanRulesEdit)
            throw CliError.InvalidParams(
                $"rulesHash 与父世代 (gen-{parent}) 不一致——人设参数世代间必须零改动",
                "若这确是人的修改：allowHumanRulesEdit=true 显式留痕；AI 不得改契约数值");

        var gen = generations.Count + 1;
        string? memoryRel = null;
        if (!string.IsNullOrWhiteSpace(memoryCsv))
        {
            var src = Path.IsPathRooted(memoryCsv) ? memoryCsv : Path.Combine(projectDir, memoryCsv);
            if (!File.Exists(src))
                throw CliError.NotFound($"memoryCsv '{memoryCsv}'", "按项目根解析");
            memoryRel = Path.GetRelativePath(projectDir,
                Path.Combine(root, $"gen-{gen}-memory.csv"));
            File.Copy(src, Path.Combine(projectDir, memoryRel), overwrite: true);
        }
        var record = new GenerationEntry
        {
            Npc = npc, Generation = gen, Parent = parent, Proposal = proposal,
            CreatedAtUtc = DateTime.UtcNow, RulesHash = rulesHash,
            Evidence = manifest.LastVerdict, Memory = memoryRel, Note = note,
        };
        File.WriteAllText(Path.Combine(root, $"gen-{gen}.json"),
            JsonSerializer.Serialize(record, new JsonSerializerOptions { WriteIndented = true }));
        return new
        {
            recorded = true,
            npc,
            generation = gen,
            parent,
            rulesHash,
            humanRulesEdit = hashDrift,   // true=留痕的人为修改
        };
    }

    [CliCommand("gameplay.generation.list",
        Description = "列 NPC 的世代链（generation/parent/proposal/rulesHash 摘要/证据与记忆指针）。" +
                  "npc 省略=列全部 NPC。只读。",
        Example = """{"npc":"Guard"}""")]
    public object GenerationList(string? npc = null)
    {
        var (_, projectDir) = RequireProject();
        var root = GenerationsRoot(projectDir);
        var result = new List<object>();
        if (Directory.Exists(root))
        {
            foreach (var dir in Directory.GetDirectories(root).OrderBy(d => d, StringComparer.Ordinal))
            {
                var name = Path.GetFileName(dir);
                if (npc != null && name != npc) continue;
                var gens = Directory.GetFiles(dir, "gen-*.json")
                    .Select(f => JsonSerializer.Deserialize<GenerationEntry>(File.ReadAllText(f),
                        new JsonSerializerOptions { PropertyNameCaseInsensitive = true }))
                    .Where(g => g != null).Select(g => g!)
                    .OrderBy(g => g.Generation).ToList();
                result.Add(new
                {
                    npc = name,
                    latest = gens.Count > 0 ? gens[^1].Generation : 0,
                    generations = gens.Select(g => new
                    {
                        g.Generation, g.Parent, g.Proposal,
                        rulesHash8 = g.RulesHash[..8],
                        g.Evidence, g.Memory,
                    }),
                });
            }
        }
        return new { npcs = result };
    }

    [CliCommand("gameplay.generation.rollback",
        Description = "世代回滚（人的最终决定权——sandbox 自治不覆盖本命令）。回滚到 parent 世代：" +
                  "提案按其 backup 恢复文件（若该提案已 applied）——世代切换点原子回退；不修改" +
                  "任何世代记录（世代链只增不改，回滚后新世代=继续演进）。失败：INVALID_PARAMS" +
                  "（世代不存在/已是首代）。",
        Example = """{"npc":"Guard"}""")]
    public object GenerationRollback(string npc)
    {
        var (_, projectDir) = RequireProject();
        var root = Path.Combine(GenerationsRoot(projectDir), npc);
        if (!Directory.Exists(root))
            throw CliError.NotFound($"NPC '{npc}' 无世代目录", "先 gameplay.generation.record");
        var gens = Directory.GetFiles(root, "gen-*.json")
            .Select(f => JsonSerializer.Deserialize<GenerationEntry>(File.ReadAllText(f),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true }))
            .Where(g => g != null).Select(g => g!).OrderBy(g => g.Generation).ToList();
        if (gens.Count == 0) throw CliError.InvalidParams("世代链为空");
        var latest = gens[^1];
        if (latest.Parent == null)
            throw CliError.InvalidParams($"gen-{latest.Generation} 已是首代", "没有可回滚的父世代");

        // 回滚=对最新提案执行 gameplay.rollback（文件备份恢复+规则前像恢复）。
        var proposalsRoot = Path.Combine(projectDir, ".Primal", "gameplay", "proposals");
        var mfPath = Path.Combine(proposalsRoot, latest.Proposal, "proposal.json");
        if (!File.Exists(mfPath))
            throw new CliError(CliErrorCodes.EngineError,
                $"世代 {latest.Generation} 的提案 '{latest.Proposal}' manifest 缺失", "世代记录与提案目录不一致");
        var manifest = JsonSerializer.Deserialize<GameplayCommands.ProposalManifest>(
                File.ReadAllText(mfPath), new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw CliError.InvalidParams("提案 manifest 损坏");
        var rolled = false;
        if (manifest.Status == "applied")
        {
            new GameplayCommands().Rollback(latest.Proposal);
            rolled = true;
        }
        return new
        {
            rolledBack = true,
            npc,
            from = latest.Generation,
            to = latest.Parent,
            proposalRestored = latest.Proposal,
            proposalRollbackExecuted = rolled,
        };
    }

    // ==== S4：多样性门禁（n-gram 重复率）====

    [CliCommand("gameplay.diversity",
        Description = "行为流 n-gram 重复率度量（多样性只作下限约束：repeatRate 须低于阈值，" +
                  "不追求最小化——防 Goodhart）。输入一个或多个（逗号分隔）trace CSV" +
                  "（--dryrun 产物；绝对或项目相对路径）：跨运行计算实体的动作流" +
                  "（trace 中 entity=<名> 的记录序），n 默认 3。跨运行重复率=跨文件重复 " +
                  "n-gram 占比；单文件时=文件内重复占比。返回逐实体 {ngrams, distinct, " +
                  "repeatRate} 与总体值。threshold 给定时 repeatRate ≥ threshold 记 gate=false" +
                  "（门禁判不过——证据门可据此拦截重复行为）。",
        Example = """{"tracesCsv":"a.csv,b.csv","n":3,"threshold":0.6}""")]
    public object Diversity(string tracesCsv, int n = 3, double threshold = 0.0)
    {
        var (_, projectDir) = RequireProject();
        if (n < 1 || n > 8) throw CliError.InvalidParams($"n={n} 越界", "1..8");
        var paths = tracesCsv.Split(',', StringSplitOptions.RemoveEmptyEntries |
                                            StringSplitOptions.TrimEntries);
        if (paths.Length == 0) throw CliError.InvalidParams("tracesCsv 为空");

        // 实体 → 逐运行动作序列（一个 trace 文件=一次运行；行解析取 entity=<名>）
        var perEntity = new Dictionary<string, List<List<string>>>();
        foreach (var raw in paths)
        {
            var path = Path.IsPathRooted(raw) ? raw : Path.Combine(projectDir, raw);
            if (!File.Exists(path))
                throw CliError.NotFound($"trace '{raw}'", "按项目根解析；用 gameplay.dryrun 产物");
            var runActions = new Dictionary<string, List<string>>();
            foreach (var line in File.ReadAllLines(path))
            {
                if (line.StartsWith('#') || line.Length == 0) continue;
                var entity = ExtractEntity(line);
                if (entity == null) continue;
                var action = CompactAction(line);
                if (!runActions.TryGetValue(entity, out var list)) runActions[entity] = list = new();
                list.Add(action);
            }
            foreach (var (entity, actions) in runActions)
            {
                if (!perEntity.TryGetValue(entity, out var runs)) perEntity[entity] = runs = new();
                runs.Add(actions);
            }
        }

        var perEntityResult = new List<object>();
        double totalRepeat = 0; int counted = 0;
        foreach (var (entity, runs) in perEntity.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            // 跨运行 n-gram：把每条 run 展开成 (runIdx 交接处断开) 的 n-gram 序列
            var grams = new List<string>();
            foreach (var run in runs)
                for (int i = 0; i + n <= run.Count; ++i)
                    grams.Add(string.Join('>', run.Skip(i).Take(n)));
            double repeat = 0;
            if (grams.Count > 0)
            {
                var distinct = grams.Distinct().Count();
                repeat = 1.0 - (double)distinct / grams.Count;
            }
            totalRepeat += repeat; ++counted;
            perEntityResult.Add(new
            {
                entity,
                runs = runs.Count,
                actions = runs.Sum(r => r.Count),
                ngrams = grams.Count,
                distinct = grams.Distinct().Count(),
                repeatRate = Math.Round(repeat, 4),
            });
        }
        var overall = counted > 0 ? totalRepeat / counted : 0;
        var gate = threshold <= 0 ? (bool?)null : overall < threshold;
        return new
        {
            n,
            runs = paths.Length,
            perEntity = perEntityResult,
            overallRepeatRate = Math.Round(overall, 4),
            threshold = threshold > 0 ? threshold : (double?)null,
            gate,
            gateSemantics = "多样性只作下限约束：repeatRate 须低于阈值（gate=true），不作最大化目标",
        };
    }

    private static readonly Regex EntityRegex = new(@"entity=([^|]+)", RegexOptions.Compiled);

    private static string? ExtractEntity(string traceLine)
    {
        var m = EntityRegex.Match(traceLine);
        return m.Success ? m.Groups[1].Value : null;
    }

    /// <summary>动作签名：kind + detail 的语义骨架（帧号剔除——跨运行时序可比）。</summary>
    private static string CompactAction(string traceLine)
    {
        var parts = traceLine.Split(',');
        if (parts.Length < 4) return traceLine;
        var kind = parts[2];
        var detail = string.Join('|', parts[3].Split('|')
            .Where(seg => !seg.StartsWith("entity=", StringComparison.Ordinal)));
        return $"{kind}:{detail}";
    }
}
