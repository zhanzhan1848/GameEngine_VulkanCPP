using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Phase 3 打磨 verbs：docs.search / workflow.run（CLI 规划 §5.4/§7.4）。
/// docs.search = ripgrep 级全文检索（结构化 schema 即检索，RAG 的第一层）；
/// workflow.run = 确定性流水线打包——Registry 里的普通命令，逐步执行既有
/// verb、stop-on-error、返回每步结果（§5.4：与单命令同一执行器）。
/// </summary>
public sealed class WorkflowCommands
{
    private readonly CliCommandRegistry _registry;

    /// <summary>workflow 步骤必须与宿主命令面同一 Registry（测试注入自己的实例）。</summary>
    public WorkflowCommands(CliCommandRegistry registry) => _registry = registry;
    private static IEnumerable<string> SearchRoots()
    {
        // 项目 Content 根（编辑器已知）+ 仓库根 Docs/（输出目录向上探测）。
        var projectRoot = AssetRegistry.ProjectRoot;
        if (!string.IsNullOrEmpty(projectRoot)) yield return projectRoot!;

        // exe 目录形如 <repo>/Darwin/PrimalEditor_Avalonia/<cfg>；逐级向上找 Docs/。
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "Docs");
            if (Directory.Exists(candidate)) yield return candidate;
        }
    }

    [CliCommand("docs.search",
        Description = "全文检索仓库文档（Docs/**.md、AGENTS.md、项目内 .md）——给 LLM 的知识检索 verb。" +
                      "query 大小写不敏感子串匹配；glob 缺省 '*.md'；limit 上限 100（默认 20）。" +
                      "返回 {path,line,text} 列表与扫描统计。纯读操作，不影响编辑器状态。",
        Example = """{"query":"退出码"}""")]
    public object DocsSearch(string query, string glob = "*.md", int limit = 20)
    {
        if (string.IsNullOrWhiteSpace(query))
            throw CliError.InvalidParams("query 不能为空", @"示例: {""query"":""undo""}");
        if (limit < 1 || limit > 100)
            throw CliError.InvalidParams("limit 必须在 1..100", @"示例: 20");
        glob ??= "*.md";

        var matches = new List<object>();
        int filesScanned = 0;
        foreach (var root in SearchRoots())
        {
            IEnumerable<string> files;
            try
            {
                files = Directory.EnumerateFiles(root, glob, SearchOption.AllDirectories)
                    .Where(f => !f.Contains("/.git/") && !f.Contains("/obj/") && !f.Contains("/bin/"));
            }
            catch (Exception) { continue; }

            foreach (var file in files)
            {
                if (matches.Count >= limit) break;
                string[] lines;
                try { lines = File.ReadAllLines(file); }
                catch (Exception) { continue; } // 二进制/被锁文件跳过
                filesScanned++;
                for (int i = 0; i < lines.Length; i++)
                {
                    if (lines[i].Contains(query, StringComparison.OrdinalIgnoreCase))
                    {
                        matches.Add(new
                        {
                            path = file,
                            line = i + 1,
                            text = lines[i].Length > 200 ? lines[i][..200] : lines[i],
                        });
                        if (matches.Count >= limit) break;
                    }
                }
            }
            if (matches.Count >= limit) break;
        }

        return new
        {
            query,
            filesScanned,
            returned = matches.Count,
            matches,
            truncated = matches.Count >= limit,
        };
    }

    [CliCommand("workflow.run",
        Description = "确定性流水线：按顺序执行 steps 数组中的命令（与 CLI 同一 Registry 执行器），" +
                      "stop-on-error——某步失败立即停止并返回已完成步骤的状态（§5.4）。" +
                      "steps 每项 {\"verb\":\"…\",\"params\":{…}}；只接受本 Registry 已注册的 verb（白名单即安全）。" +
                      "steps 内联传入，或 file 给出 JSON 文件路径。编辑类步骤会进 undo 栈，与手工执行一致。",
        Example = """{"steps":[{"verb":"scene.tree"},{"verb":"editor.ping"}]}""")]
    public object WorkflowRun(string? file = null, WorkflowStep[]? steps = null)
    {
        if (steps == null || steps.Length == 0)
        {
            if (string.IsNullOrWhiteSpace(file))
                throw CliError.InvalidParams("需要 steps 数组或 file 路径",
                    @"示例: {""steps"":[{""verb"":""editor.ping""}]}");
            var resolved = Path.IsPathRooted(file!) ? file! : Path.GetFullPath(file!);
            if (!File.Exists(resolved))
                throw CliError.NotFound($"workflow 文件 '{file}'", "传入绝对路径，或先检查工作目录");
            var content = File.ReadAllText(resolved);
            if (resolved.EndsWith(".yaml", StringComparison.OrdinalIgnoreCase)
                || resolved.EndsWith(".yml", StringComparison.OrdinalIgnoreCase))
            {
                // YAML 前端（受限子集，见 WorkflowYaml）→ 复用同一执行器
                var parsed = WorkflowYaml.Parse(content);
                steps = parsed.Select(t => new WorkflowStep
                {
                    Verb = t.Verb,
                    Params = t.Params,
                }).ToArray();
            }
            else
            {
                try
                {
                    steps = System.Text.Json.JsonSerializer.Deserialize<WorkflowStep[]>(content);
                }
                catch (Exception ex)
                {
                    throw CliError.InvalidParams($"workflow 文件解析失败: {ex.Message}",
                        @"文件内容应为 [{""verb"":""…"",""params"":{…}}, …] JSON 数组，或 .yaml（受限子集）");
                }
            }
            if (steps == null || steps.Length == 0)
                throw CliError.InvalidParams("workflow 文件中没有步骤", "至少一个 {verb, params} 步骤");
        }
        if (steps!.Length > 64)
            throw CliError.InvalidParams("steps 上限 64", "拆分成多次 workflow.run");

        var results = new List<object>();
        foreach (var (step, index) in steps!.Select((s, i) => (s, i)))
        {
            if (step == null || string.IsNullOrWhiteSpace(step.Verb))
                throw CliError.InvalidParams($"steps[{index}] 缺少 verb", @"每项形如 {""verb"":""editor.ping""}");
            if (_registry.Find(step.Verb!) == null)
                throw CliError.NotFound($"verb '{step.Verb}'",
                    "用 editor.schema 查看可用命令（注意 workflow 只能调用已注册 verb）");

            var response = _registry.InvokeAsync(new Primal.CliFramework.CliRequest
            {
                Id = $"wf-{index}",
                Verb = step.Verb!,
                Params = step.Params ?? System.Text.Json.JsonSerializer.SerializeToElement(new { }),
            }).GetAwaiter().GetResult();

            results.Add(new
            {
                index,
                verb = step.Verb,
                ok = response.Ok,
                error = response.Error,
                data = response.Ok ? response.Data : (System.Text.Json.JsonElement?)null,
            });
            if (!response.Ok)
            {
                return new
                {
                    completed = false,
                    executed = results.Count,
                    total = steps.Length,
                    failedAt = index,
                    failedVerb = step.Verb,
                    steps = results,
                    hint = "stop-on-error：失败步骤之后的步骤未执行；已执行步骤不自动回滚（可用 editor.undo 逐级撤销）",
                };
            }
        }

        return new { completed = true, executed = results.Count, total = steps.Length, steps = results };
    }

    /// <summary>workflow 步骤（verb + 参数对象）。struct 不可空校验放运行期。</summary>
    [Primal.CliFramework.CliCommandObject]
    public sealed class WorkflowStep
    {
        [System.Text.Json.Serialization.JsonPropertyName("verb")]
        public string? Verb { get; set; }

        [System.Text.Json.Serialization.JsonPropertyName("params")]
        public System.Text.Json.JsonElement? Params { get; set; }
    }
}
