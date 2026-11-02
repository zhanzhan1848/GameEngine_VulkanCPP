using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// gameplay S2（gameplay-rules.md）：人在环共创回路的命令面。
///
/// 三道门的 CLI 侧落点：
///   ①提案门  gameplay.propose  —— AI 产出永远是提案：Lua 文件 + 可选规则
///            patch 落 &lt;project&gt;/.Primal/gameplay/proposals/&lt;name&gt;/，不直接生效；
///   ②证据门  gameplay.dryrun   —— 固定步长（1/60）跑 PrimalPlayer N 帧，
///            trace CSV + verdict.json 出行为证据（含帧数/时长预算与实际值）；
///   ③确认门  gameplay.apply    —— 人执行才原子切换（备份先行，可 rollback）。
/// 确定面：dry-run 同包同帧数两轮 CSV 逐位一致（引擎侧回放语义）。
/// 改动归属：规则 patch 应用走文档模型 + UndoRedo（与 UI 同路径）；
/// AI 不得直接改契约数值——它只能提案，apply 是人的动作。
/// </summary>
public sealed class GameplayCommands
{
    // ==== 提案清单（proposals/&lt;name&gt;/proposal.json）====
    public sealed class ProposalManifest
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("note")] public string? Note { get; set; }
        [JsonPropertyName("createdAtUtc")] public DateTime CreatedAtUtc { get; set; }
        [JsonPropertyName("status")] public string Status { get; set; } = "proposed";
        // 相对项目的路径（如 Content/Scripts/door.lua）——apply 时的落点。
        [JsonPropertyName("files")] public List<string> Files { get; set; } = new();
        // 可选规则 patch：[{entity:"名", rules:{GameplayRulesComponent 字段}}]。
        // apply 时按实体名替换/追加组件（文档模型 + Undo）。
        [JsonPropertyName("rulesPatch")] public List<RulesPatchEntry>? RulesPatch { get; set; }
        [JsonPropertyName("appliedAtUtc")] public DateTime? AppliedAtUtc { get; set; }
        [JsonPropertyName("backupDir")] public string? BackupDir { get; set; }
        // apply 时的组件前像（rollback 用；与 Undo 栈独立——跨会话可回滚）。
        [JsonPropertyName("preImages")] public List<PreImage>? PreImages { get; set; }
        [JsonPropertyName("lastVerdict")] public string? LastVerdict { get; set; }
    }

    public sealed class RulesPatchEntry
    {
        [JsonPropertyName("entity")] public string Entity { get; set; } = "";
        [JsonPropertyName("rules")] public JsonElement Rules { get; set; }
    }

    public sealed class PreImage
    {
        [JsonPropertyName("entity")] public string Entity { get; set; } = "";
        // null = 原本没有 GameplayRulesComponent（rollback 时移除）。
        [JsonPropertyName("rules")] public JsonElement? Rules { get; set; }
    }

    private static string ProposalsRoot(string projectDir) =>
        Path.Combine(projectDir, ".Primal", "gameplay", "proposals");

    private static string ProposalDir(string projectDir, string name) =>
        Path.Combine(ProposalsRoot(projectDir), name);

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

    private static void ValidateName(string name)
    {
        if (string.IsNullOrWhiteSpace(name) ||
            name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.Contains(' '))
            throw CliError.InvalidParams($"提案名 '{name}' 非法",
                "非空、不含空格与文件系统非法字符（建议 kebab-case）");
    }

    private static ProposalManifest LoadManifest(string projectDir, string name)
    {
        var path = Path.Combine(ProposalDir(projectDir, name), "proposal.json");
        if (!File.Exists(path))
            throw CliError.NotFound($"提案 '{name}'", "先 gameplay.propose 创建提案");
        return JsonSerializer.Deserialize<ProposalManifest>(File.ReadAllText(path),
                   new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
               ?? throw CliError.InvalidParams($"提案 '{name}' 的 manifest 损坏", "重新 propose");
    }

    private static void SaveManifest(string projectDir, ProposalManifest m)
    {
        var dir = ProposalDir(projectDir, m.Name);
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "proposal.json"),
            JsonSerializer.Serialize(m, new JsonSerializerOptions { WriteIndented = true }));
    }

    // ==== ① 提案门 ====

    [CliCommand("gameplay.propose",
        Description = "登记一份玩法提案（AI 产出的唯一进入形式——不直接生效）。filesCsv 为逗号分隔的" +
                      "文件条目，两种形态：'路径'（绝对或项目相对——apply 落点=其项目相对路径，适合" +
                      "替换既有脚本）或 '源|目标'（源任意可达路径，目标=项目相对 .lua——变体放临时处" +
                      "不留工作区）。文件复制进提案 files/ 下按目标路径存档。rulesPatchJson 可选：" +
                      "JSON 数组 [{entity:'实体名',rules:{InitialState/States/Transitions/Triggers/" +
                      "Tasks 字段}}]——apply 时按实体名替换/追加 GameplayRulesComponent（文档模型+" +
                      "Undo）。同名提案覆盖（重新提案）。失败：NOT_READY（无项目/场景）、" +
                      "INVALID_PARAMS（名/文件非法）、NOT_FOUND（文件不存在）。",
        Example = """{"name":"door-faster","filesCsv":"/tmp/door_v2.lua|Content/Scripts/door.lua"}""")]
    public object Propose(string name, string filesCsv, string? note = null, string? rulesPatchJson = null)
    {
        ValidateName(name);
        var (_, projectDir) = RequireProject();
        var files = (filesCsv ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries |
                                           StringSplitOptions.TrimEntries);
        if (files.Length == 0)
            throw CliError.InvalidParams("filesCsv 为空", "至少提供一个 .lua 文件条目");

        var dir = ProposalDir(projectDir, name);
        var filesDir = Path.Combine(dir, "files");
        if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        Directory.CreateDirectory(filesDir);

        var staged = new List<string>();
        foreach (var entry in files)
        {
            var bar = entry.IndexOf('|');
            var srcRaw = bar >= 0 ? entry[..bar].Trim() : entry;
            var dstRel = bar >= 0 ? entry[(bar + 1)..].Trim() : null;
            if (!srcRaw.EndsWith(".lua", StringComparison.OrdinalIgnoreCase) ||
                (dstRel != null && !dstRel.EndsWith(".lua", StringComparison.OrdinalIgnoreCase)))
                throw CliError.InvalidParams($"条目 '{entry}' 不是 .lua", "提案文件仅接受 Lua 行为模块");
            var srcAbs = Path.IsPathRooted(srcRaw) ? srcRaw : Path.Combine(projectDir, srcRaw);
            if (!File.Exists(srcAbs))
                throw CliError.NotFound($"源文件 '{srcRaw}'", "路径按项目根解析（或绝对路径）");
            string relNorm;
            if (dstRel != null)
            {
                relNorm = dstRel.Replace('\\', '/');
                var relCheck = Path.GetRelativePath(projectDir, Path.Combine(projectDir, dstRel));
                if (relCheck.StartsWith("..", StringComparison.Ordinal))
                    throw CliError.InvalidParams($"目标 '{dstRel}' 在项目外", "目标必须是项目相对路径");
            }
            else
            {
                var rel = Path.GetRelativePath(projectDir, srcAbs);
                if (rel.StartsWith("..", StringComparison.Ordinal))
                    throw CliError.InvalidParams($"文件 '{srcRaw}' 在项目外且未给目标",
                        "项目外变体用 '源|目标' 形态指定落点");
                relNorm = rel.Replace('\\', '/');
            }
            var dest = Path.Combine(filesDir, relNorm);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(srcAbs, dest, overwrite: true);
            staged.Add(relNorm);
        }

        List<RulesPatchEntry>? patch = null;
        if (!string.IsNullOrWhiteSpace(rulesPatchJson))
        {
            try
            {
                patch = JsonSerializer.Deserialize<List<RulesPatchEntry>>(rulesPatchJson,
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            }
            catch (JsonException ex)
            {
                throw CliError.InvalidParams($"rulesPatchJson 解析失败：{ex.Message}",
                    "形如 [{\"entity\":\"Door\",\"rules\":{\"InitialState\":\"closed\"}}]");
            }
            if (patch is not { Count: > 0 })
                throw CliError.InvalidParams("rulesPatchJson 为空数组", "省略该参数或提供至少一条");
        }

        var manifest = new ProposalManifest
        {
            Name = name, Note = note, CreatedAtUtc = DateTime.UtcNow,
            Files = staged, RulesPatch = patch, Status = "proposed",
        };
        SaveManifest(projectDir, manifest);
        return new
        {
            proposal = name,
            status = manifest.Status,
            files = staged,
            rulesPatchEntries = patch?.Count ?? 0,
            dir,
            effective = false,
            hint = "提案未生效；gameplay.dryrun 取证，人确认后 gameplay.apply 原子切换",
        };
    }

    // ==== ② 证据门 ====

    [CliCommand("gameplay.dryrun",
        Description = "对提案跑固定步长（1/60）dry-run 出行为证据。导出当前场景包到临时目录，" +
                      "把提案 Lua 覆盖进包旁同名相对路径（Player 解析包相对优先），并（若有）把规则" +
                      "patch 应用到场景克隆后导出——激活场景与项目文件零改动。启动 PrimalPlayer " +
                      "--dryrun frames --trace-out，等进程退出（timeoutSec 预算，超时杀进程并在 " +
                      "verdict 标记）。产物：trace.csv（确定性口径 index/frame/kind/detail，两轮逐位" +
                      "一致）+ verdict.json（证据指针/预算与实际值/事件直方图）。失败：NOT_FOUND" +
                      "（提案不存在）、INVALID_PARAMS（帧数/超时非法、提案已 applied）、ENGINE_ERROR" +
                      "（Player 缺失/退出码非 0——细节看 stderr.log）。",
        Example = """{"name":"door-faster","frames":150,"timeoutSec":120}""")]
    public object DryRun(string name, int frames = 150, int timeoutSec = 120)
    {
        ValidateName(name);
        var (worldEditor, projectDir) = RequireProject();
        if (frames < 1 || frames > 100_000)
            throw CliError.InvalidParams($"frames={frames} 越界", "1..100000");
        if (timeoutSec < 5 || timeoutSec > 3600)
            throw CliError.InvalidParams($"timeoutSec={timeoutSec} 越界", "5..3600（时长预算）");

        var manifest = LoadManifest(projectDir, name);
        if (manifest.Status == "applied")
            throw CliError.InvalidParams($"提案 '{name}' 已 applied",
                "dry-run 面向未生效提案；回滚后可重验");

        // Player 二进制：<repoRoot>/<ResolvePlayerDir(api, config)>/PrimalPlayer
        var repoRoot = RuntimeLauncher.ResolveRepoRoot()
            ?? throw new CliError(CliErrorCodes.EngineError, "仓库根目录未解析",
                "PRIMAL_REPO_ROOT 或编辑器标准目录结构");
        var config = worldEditor.Project?.BuildConfigName ?? "Debug";
        var playerDir = RuntimeLauncher.ResolvePlayerDir(worldEditor.TargetGraphicsApi, config);
        var playerPath = Path.Combine(repoRoot, playerDir, "PrimalPlayer");
        if (!File.Exists(playerPath))
            throw new CliError(CliErrorCodes.EngineError, $"PrimalPlayer 缺失：{playerPath}",
                $"先构建: cmake --build {playerDir} --target PrimalPlayer");

        // 工作目录：<project>/.Primal/gameplay/dryrun/<name>-<时间戳>/
        var workDir = Path.Combine(projectDir, ".Primal", "gameplay", "dryrun",
            $"{name}-{DateTime.Now:yyyyMMdd-HHmmss}");
        Directory.CreateDirectory(workDir);

        // 1) 场景克隆 + 规则 patch → 导出 → 包挪进工作目录。
        var sceneClone = Serializer.FromSceneDocument(Serializer.ToSceneDocument(worldEditor.ActiveScene));
        if (manifest.RulesPatch is { Count: > 0 })
        {
            foreach (var entry in manifest.RulesPatch)
                ApplyPatchToScene(sceneClone, entry.Entity, entry.Rules);
        }
        var exported = PrimalEditor_Avalonia.Content.RuntimeSceneExporter.Export(
            sceneClone, projectDir, portable: false, Sequence.SequenceEditorState.Current)
            ?? throw new CliError(CliErrorCodes.EngineError, "场景包导出失败", "查看编辑器日志");
        var pkgPath = Path.Combine(workDir, "pkg.primalrun");
        File.Move(exported, pkgPath, overwrite: true);

        // 2) 提案 Lua 覆盖到包旁同名相对路径（ResolveAssetPath 包相对优先）。
        var filesDir = Path.Combine(ProposalDir(projectDir, name), "files");
        foreach (var rel in manifest.Files)
        {
            var src = Path.Combine(filesDir, rel);
            var dest = Path.Combine(workDir, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(src, dest, overwrite: true);
        }

        // 3) 跑 Player（固定步长预算在 Player 内部；这里再加墙钟预算）。
        var traceCsv = Path.Combine(workDir, "trace.csv");
        var stderrLog = Path.Combine(workDir, "stderr.log");
        var sw = Stopwatch.StartNew();
        var timedOut = false;
        int exitCode;
        using (var proc = new Process())
        {
            proc.StartInfo = new ProcessStartInfo(playerPath)
            {
                Arguments = $"--scene \"{pkgPath}\" --dryrun {frames} --trace-out \"{traceCsv}\"",
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            proc.Start();
            var stderr = proc.StandardError.ReadToEnd();
            File.WriteAllText(stderrLog, stderr);
            if (!proc.WaitForExit(timeoutSec * 1000))
            {
                timedOut = true;
                proc.Kill(entireProcessTree: true);
                proc.WaitForExit(5000);
            }
            exitCode = proc.ExitCode;
        }
        var wallMs = sw.ElapsedMilliseconds;

        // 4) verdict：事件直方图 + 证据指针 + 预算对账。
        var records = new List<string>();
        var histogram = new Dictionary<string, int>();
        if (File.Exists(traceCsv))
        {
            foreach (var line in File.ReadAllLines(traceCsv))
            {
                if (line.StartsWith('#')) continue;
                records.Add(line);
                var kind = line.Split(',').ElementAtOrDefault(2);
                if (kind != null)
                {
                    var label = kind switch
                    {
                        "0" => "trigger_fired", "1" => "state_auto", "2" => "state_lua",
                        "3" => "build_error", "4" => "task_completed", "5" => "task_failed",
                        "6" => "warn", _ => kind,
                    };
                    histogram[label] = histogram.GetValueOrDefault(label) + 1;
                }
            }
        }
        var verdict = new
        {
            proposal = name,
            ok = !timedOut && exitCode == 0 && records.Count > 0,
            timedOut,
            exitCode,
            framesBudget = frames,
            timeoutBudgetSec = timeoutSec,
            wallMs,
            records = records.Count,
            firstFrame = records.Count > 0 ? records[0] : null,
            lastFrame = records.Count > 0 ? records[^1] : null,
            histogram,
            evidence = new
            {
                package = Path.GetRelativePath(projectDir, pkgPath),
                traceCsv = Path.GetRelativePath(projectDir, traceCsv),
                stderr = Path.GetRelativePath(projectDir, stderrLog),
                verdictJson = "见本命令返回；副本已写入 verdict.json",
            },
            determinism = "同提案同帧数两轮 dry-run 的 trace.csv 逐位一致（引擎回放语义）",
        };
        var verdictPath = Path.Combine(workDir, "verdict.json");
        File.WriteAllText(verdictPath, JsonSerializer.Serialize(verdict,
            new JsonSerializerOptions { WriteIndented = true }));

        manifest.LastVerdict = Path.GetRelativePath(projectDir, verdictPath);
        SaveManifest(projectDir, manifest);

        if (timedOut || exitCode != 0)
            throw new CliError(CliErrorCodes.EngineError,
                $"dry-run 未完成（timedOut={timedOut}, exit={exitCode}）",
                $"证据已保留：{stderrLog}");
        return verdict;
    }

    // ==== ③ 确认门 ====

    [CliCommand("gameplay.apply",
        Description = "人确认后原子生效提案（确认门的执行动作；由人发起，AI 不得调用）。文件：备份" +
                      "原件到 proposals/<name>/backup-<时间戳>/ 后覆盖；规则 patch：按实体名替换/追加 " +
                      "GameplayRulesComponent（文档模型+UndoRedo，前像记入 manifest 供跨会话回滚）。" +
                      "建议先 gameplay.dryrun 看证据（有 verdict 记录时返回里带 evidence）。失败：" +
                      "NOT_FOUND（提案不存在）、INVALID_PARAMS（已 applied / patch 实体不在场景）。" +
                      "原子性：任一文件复制失败即整体中止（已备份未覆盖的不动），已覆盖部分回滚。",
        Example = """{"name":"door-faster"}""")]
    public object Apply(string name)
    {
        ValidateName(name);
        var (worldEditor, projectDir) = RequireProject();
        var manifest = LoadManifest(projectDir, name);
        if (manifest.Status == "applied")
            throw CliError.InvalidParams($"提案 '{name}' 已 applied", "如需重应用先 gameplay.rollback");
        if (manifest.Status == "rejected")
            throw CliError.InvalidParams($"提案 '{name}' 已被驳回",
                "迭代请同名 re-propose（覆盖历史），直接 apply 被驳回状态拦住");

        // 1) 备份 + 覆盖文件（staged→move 保原子；任一失败回滚已 move 的）。
        var backupDir = Path.Combine(ProposalDir(projectDir, name),
            $"backup-{DateTime.Now:yyyyMMdd-HHmmss}");
        var moved = new List<(string src, string dest, string? backup)>();
        try
        {
            foreach (var rel in manifest.Files)
            {
                var src = Path.Combine(ProposalDir(projectDir, name), "files", rel);
                var dest = Path.Combine(projectDir, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                Directory.CreateDirectory(backupDir);
                string? backup = null;
                if (File.Exists(dest))
                {
                    backup = Path.Combine(backupDir, rel);
                    Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                    File.Copy(dest, backup, overwrite: true);
                }
                File.Copy(src, dest, overwrite: true);
                moved.Add((src, dest, backup));
            }
        }
        catch (Exception)
        {
            // 原子性回滚：恢复已有备份 / 删除新落文件
            foreach (var (_, dest, backup) in moved)
            {
                if (backup != null) File.Copy(backup, dest, overwrite: true);
                else if (File.Exists(dest)) File.Delete(dest);
            }
            throw;
        }

        // 2) 规则 patch：文档模型 + Undo；前像记 manifest（跨会话回滚）。
        var preImages = new List<PreImage>();
        if (manifest.RulesPatch is { Count: > 0 })
        {
            foreach (var entry in manifest.RulesPatch)
            {
                var (entity, old) = ApplyPatchToSceneWithUndo(worldEditor, entry.Entity, entry.Rules);
                preImages.Add(new PreImage
                {
                    Entity = entry.Entity,
                    Rules = old,
                });
            }
            worldEditor.MarkProjectDirty();
        }

        manifest.Status = "applied";
        manifest.AppliedAtUtc = DateTime.UtcNow;
        manifest.BackupDir = Path.GetRelativePath(projectDir, backupDir);
        manifest.PreImages = preImages;
        SaveManifest(projectDir, manifest);
        return new
        {
            applied = true,
            proposal = name,
            files = manifest.Files,
            rulesPatched = preImages.Count,
            backupDir = manifest.BackupDir,
            lastVerdict = manifest.LastVerdict,
            rollbackHint = $"gameplay.rollback {name}",
        };
    }

    [CliCommand("gameplay.rollback",
        Description = "回滚已 applied 的提案：文件从备份恢复（无原作的删除）；规则 patch 按前像恢复" +
                      "（文档模型+Undo 记录恢复动作）。幂等边界：提案未 applied 返回 INVALID_PARAMS。" +
                      "回滚后状态回 proposed（可再改再验）。",
        Example = """{"name":"door-faster"}""")]
    public object Rollback(string name)
    {
        ValidateName(name);
        var (worldEditor, projectDir) = RequireProject();
        var manifest = LoadManifest(projectDir, name);
        if (manifest.Status != "applied")
            throw CliError.InvalidParams($"提案 '{name}' 未 applied", "只有已生效提案可回滚");

        var restored = 0;
        if (manifest.BackupDir != null)
        {
            var backupRoot = Path.Combine(projectDir, manifest.BackupDir);
            foreach (var rel in manifest.Files)
            {
                var dest = Path.Combine(projectDir, rel);
                var backup = Path.Combine(backupRoot, rel);
                if (File.Exists(backup)) File.Copy(backup, dest, overwrite: true);
                else if (File.Exists(dest)) File.Delete(dest);
                restored++;
            }
        }

        var rulesRestored = 0;
        if (manifest.PreImages is { Count: > 0 })
        {
            foreach (var pre in manifest.PreImages)
                ApplyPreImageToScene(worldEditor, pre);
            worldEditor.MarkProjectDirty();
            rulesRestored = manifest.PreImages.Count;
        }

        manifest.Status = "proposed";
        manifest.AppliedAtUtc = null;
        manifest.BackupDir = null;
        manifest.PreImages = null;
        SaveManifest(projectDir, manifest);
        return new { rolledBack = true, proposal = name, filesRestored = restored, rulesRestored };
    }

    [CliCommand("gameplay.rules.attach",
        Description = "给实体挂 GameplayRulesComponent（已有则幂等返回）。空契约起步（InitialState" +
                      "空=仅触发器段）；编辑走 Inspector GAMEPLAY RULES 段（JSON，文档模型+Undo）" +
                      "或提案 patch。改动走 UndoRedo。失败：NOT_FOUND（实体不存在）。",
        Example = """{"entityGuid":"…"}""")]
    public object RulesAttach(Guid entityGuid)
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        var entity = FindEntityDeep(worldEditor.ActiveScene!.Entities, entityGuid)
            ?? throw CliError.NotFound($"实体 {entityGuid}", "实体不在激活场景");
        var existing = entity.GetComponent<GameplayRulesComponent>();
        if (existing != null)
            return new { attached = false, reason = "already-present", entity = entity.Name };
        var comp = new GameplayRulesComponent();
        entity.Components.Add(comp);
        worldEditor.UndoRedo.Add(new DelegateAction("gameplay.rules.attach",
            () => entity.Components.Remove(comp),
            () =>
            {
                if (!entity.Components.Contains(comp)) entity.Components.Add(comp);
            }));
        worldEditor.MarkProjectDirty();
        return new { attached = true, entity = entity.Name, hint = "Inspector → GAMEPLAY RULES 编辑契约 JSON（应用=文档模型+Undo）" };
    }

    [CliCommand("gameplay.review.open",
        Description = "打开共创回路决定面窗口（S3 确认面的 UI 壳）：逐提案三个动作——" +
                      "批准生效（=gameplay.apply）/ 驳回（必填原因，回喂 AI）/ 重新取证（=dryrun）。" +
                      "只调参数不在此窗（人经 Inspector/组件命令改 L0 数值+Undo）。幂等：已开则置前。" +
                      "失败：NOT_READY（无项目/UI 未就绪）。",
        Example = "{}")]
    public object ReviewOpen()
    {
        var (_, _) = RequireProject();
        if (EditorServices.MainWindow == null)
            throw CliError.NotReady("主窗口未就绪", "编辑器完全启动后重试");
        var opened = Avalonia.Threading.Dispatcher.UIThread.InvokeAsync(() =>
        {
            Gameplay.GameplayReviewWindow? found = null;
            var desktop = Avalonia.Application.Current?.ApplicationLifetime
                as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
            if (desktop != null)
                foreach (var w in desktop.Windows)
                    if (w is Gameplay.GameplayReviewWindow existing) { found = existing; break; }
            if (found != null) { found.Activate(); found.Refresh(); }
            else
            {
                var win = new Gameplay.GameplayReviewWindow();
                win.Show(EditorServices.MainWindow);
            }
            return true;
        }).GetAwaiter().GetResult();
        return new { opened, window = "GameplayReviewWindow", decisionSurface = new[] { "approve", "reject+reason", "re-evidence" } };
    }

    [CliCommand("gameplay.status",
        Description = "列出全部提案与状态（proposed/applied）、文件数、是否有 dry-run 证据。" +
                      "附目录位置。只读。",
        Example = "{}")]
    public object Status()
    {
        var (_, projectDir) = RequireProject();
        var root = ProposalsRoot(projectDir);
        var items = new List<object>();
        if (Directory.Exists(root))
        {
            foreach (var dir in Directory.GetDirectories(root).OrderBy(d => d, StringComparer.Ordinal))
            {
                var mfPath = Path.Combine(dir, "proposal.json");
                if (!File.Exists(mfPath)) continue;
                var mf = JsonSerializer.Deserialize<ProposalManifest>(File.ReadAllText(mfPath),
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                if (mf == null) continue;
                items.Add(new
                {
                    name = mf.Name,
                    status = mf.Status,
                    files = mf.Files.Count,
                    rulesPatch = mf.RulesPatch?.Count ?? 0,
                    hasVerdict = mf.LastVerdict != null,
                    appliedAtUtc = mf.AppliedAtUtc,
                });
            }
        }
        return new { proposals = items, root };
    }

    // ==== 内部：patch 应用（克隆用，无 Undo）====

    private static void ApplyPatchToScene(Scene scene, string entityName, JsonElement rules)
    {
        var entity = FindEntity(scene.Entities, entityName)
            ?? throw CliError.NotFound($"patch 目标实体 '{entityName}'", "场景中无此名字的实体");
        var rulesComp = DeserializeRules(rules);
        var existing = entity.GetComponent<GameplayRulesComponent>();
        if (existing != null) entity.Components.Remove(existing);
        entity.Components.Add(rulesComp);
    }

    // ==== 内部：patch 应用（活场景，文档模型 + Undo，返回前像）====

    private static (GameEntity entity, JsonElement? preImage) ApplyPatchToSceneWithUndo(
        WorldEditorViewModel worldEditor, string entityName, JsonElement rules)
    {
        var entity = FindEntity(worldEditor.ActiveScene!.Entities, entityName)
            ?? throw CliError.NotFound($"patch 目标实体 '{entityName}' 不在激活场景",
                "先 scene.load 正确场景或修正 patch 的 entity 名");
        var old = entity.GetComponent<GameplayRulesComponent>();
        JsonElement? preImage = null;
        if (old != null)
            preImage = JsonSerializer.SerializeToElement(old, SceneDocument.JsonOptions);
        var newComp = DeserializeRules(rules);

        if (old != null) entity.Components.Remove(old);
        entity.Components.Add(newComp);
        worldEditor.UndoRedo.Add(new DelegateAction("gameplay.apply",
            () =>
            {
                entity.Components.Remove(newComp);
                if (old != null && !entity.Components.Contains(old)) entity.Components.Add(old);
            },
            () =>
            {
                if (old != null) entity.Components.Remove(old);
                if (!entity.Components.Contains(newComp)) entity.Components.Add(newComp);
            }));
        return (entity, preImage);
    }

    private static void ApplyPreImageToScene(WorldEditorViewModel worldEditor, PreImage pre)
    {
        var entity = FindEntity(worldEditor.ActiveScene!.Entities, pre.Entity);
        if (entity == null) return;
        var current = entity.GetComponent<GameplayRulesComponent>();
        if (current != null) entity.Components.Remove(current);
        if (pre.Rules is { } img)
        {
            var restored = DeserializeRules(img);
            entity.Components.Add(restored);
            worldEditor.UndoRedo.Add(new DelegateAction("gameplay.rollback",
                () => entity.Components.Remove(restored),
                () =>
                {
                    if (!entity.Components.Contains(restored)) entity.Components.Add(restored);
                }));
        }
    }

    private static GameplayRulesComponent DeserializeRules(JsonElement rules)
    {
        var comp = JsonSerializer.Deserialize<GameplayRulesComponent>(rules.GetRawText(),
            SceneDocument.JsonOptions);
        if (comp == null)
            throw CliError.InvalidParams("rules 反序列化失败", "字段见 GameplayRulesComponent");
        return comp;
    }

    private static GameEntity? FindEntityDeep(IEnumerable<GameEntity> roots, Guid guid)
    {
        foreach (var e in roots)
        {
            if (e.EntityGuid.Value == guid) return e;
            var hit = FindEntityDeep(e.Children, guid);
            if (hit != null) return hit;
        }
        return null;
    }

    private static GameEntity? FindEntity(IEnumerable<GameEntity> roots, string name)
    {
        foreach (var e in roots)
        {
            if (string.Equals(e.Name, name, StringComparison.Ordinal)) return e;
            var hit = FindEntity(e.Children, name);
            if (hit != null) return hit;
        }
        return null;
    }
}
