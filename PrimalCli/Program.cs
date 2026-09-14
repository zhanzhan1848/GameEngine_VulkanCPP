using System;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Primal.CliFramework;

namespace PrimalCli;

/// <summary>
/// primal — thin client for the engine CLI / Agent interface (Phase 0).
///
/// Output contract (plan §6.2):
///   stdout: machine-readable JSON only (the response, pretty-printed)
///   stderr: human diagnostics
///   exit:   0 ok / 2 usage·param error / 3 not found / 4 engine·internal / 5 editor not running
///
/// Usage:
///   primal <verb> ['{"param":value,...}'] [--socket path] [--timeout ms] [--wait sec]
///   primal discover                 list live editor sockets
///   primal selftest                 in-process framework roundtrip (no editor needed)
/// </summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        try
        {
            return Run(args);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"primal: {ex.Message}");
            return CliExitCodes.UsageError;
        }
    }

    private static int Run(string[] args)
    {
        string? socketPath = null;
        int timeoutMs = CliClient.DefaultTimeoutMs;
        int waitSeconds = 0;
        var positional = new System.Collections.Generic.List<string>();

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--socket" when i + 1 < args.Length:
                    socketPath = args[++i];
                    break;
                case "--timeout" when i + 1 < args.Length && int.TryParse(args[++i], out var t):
                    timeoutMs = t;
                    break;
                case "--wait" when i + 1 < args.Length && int.TryParse(args[++i], out var w):
                    waitSeconds = w;
                    break;
                case "-h" or "--help":
                    PrintUsage();
                    return CliExitCodes.Success;
                default:
                    positional.Add(args[i]);
                    break;
            }
        }

        if (positional.Count == 0)
        {
            PrintUsage();
            return CliExitCodes.UsageError;
        }

        var verb = positional[0];
        switch (verb)
        {
            case "discover":
                return Discover();
            case "selftest":
                return SelfTest.RunAll();
            case "schema":
                return AggregateSchema().GetAwaiter().GetResult();
        }

        // 离线路由（规划 §3.1/§7.2）：offline verb 不走 socket，交给
        // primal-engine 子进程。路由表白名单维护（离线 verb 少且稳定；
        // 将来由聚合 schema 的 channel 标注驱动）。
        if (verb is "render" or "engine" or "bake-vt" or "bake-gi" or "build-shaders" or "cook-mesh")
        {
            return RouteOffline(verb, positional.Skip(1).ToList());
        }

        // test.run 的离线渲染基线门禁（§8）：`primal test render …`
        if (verb == "test" && positional.Count >= 2 && positional[1] == "render")
        {
            return TestRender(positional.Skip(2).ToList());
        }

        // M2 T3 离线物理门禁：`primal test physics …`（physics-sim 离屏仿真 + PNG 基线）
        if (verb == "test" && positional.Count >= 2 && positional[1] == "physics")
        {
            return TestPhysics(positional.Skip(2).ToList());
        }

        // Optional params JSON: either argv[1] or '-' (read stdin, for long payloads).
        JsonElement? parameters = null;
        if (positional.Count >= 2)
        {
            var raw = positional[1] == "-" ? Console.In.ReadToEnd() : positional[1];
            try
            {
                parameters = JsonSerializer.Deserialize<JsonElement>(raw);
                if (parameters!.Value.ValueKind is not (JsonValueKind.Object or JsonValueKind.Null))
                    throw new JsonException("params 必须是 JSON 对象");
            }
            catch (JsonException je)
            {
                Console.Error.WriteLine($"primal: 参数 JSON 无效: {je.Message}");
                Console.Error.WriteLine($"  期望形如 {verb} '{{\"name\":value}}'");
                return CliExitCodes.UsageError;
            }
        }

        return InvokeCommand(verb, parameters, socketPath, timeoutMs, waitSeconds).GetAwaiter().GetResult();
    }

    private static async Task<int> InvokeCommand(
        string verb, JsonElement? parameters, string? socketPath, int timeoutMs, int waitSeconds)
    {
        // --wait N: poll for an editor to appear (CI smoke convenience).
        if (socketPath == null && waitSeconds > 0)
        {
            var deadline = DateTime.UtcNow.AddSeconds(waitSeconds);
            while (CliClient.DiscoverSocketPaths().Count == 0 && DateTime.UtcNow < deadline)
                await Task.Delay(500).ConfigureAwait(false);
        }

        var (response, usedPath) = await CliClient.InvokeOnceAsync(verb, parameters, timeoutMs, socketPath).ConfigureAwait(false);
        _ = usedPath;

        Console.Out.WriteLine(JsonSerializer.Serialize(response, PrettyOptions));

        if (response.Ok) return CliExitCodes.Success;

        var err = response.Error ?? new CliErrorPayload { Code = CliErrorCodes.Internal, Message = "no error payload" };
        Console.Error.WriteLine($"primal: {err.Code}: {err.Message}");
        if (!string.IsNullOrEmpty(err.Hint))
            Console.Error.WriteLine($"  hint: {err.Hint}");

        return MapExitCode(err.Code);
    }

    /// <summary>Shared output options: pretty + UTF-8 直出（与协议层一致）。</summary>
    private static JsonSerializerOptions PrettyOptions { get; } = new()
    {
        WriteIndented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private static int MapExitCode(string code) => code switch
    {
        CliErrorCodes.NotConnected => CliExitCodes.EditorNotRunning,
        CliErrorCodes.NotFound => CliExitCodes.NotFound,
        CliErrorCodes.InvalidParams or CliErrorCodes.BadVerb or CliErrorCodes.BadRequest => CliExitCodes.UsageError,
        _ => CliExitCodes.EngineError,
    };

    // === 离线路由（§3.1：offline verb = one-shot 子进程） ===

    /// <summary>定位 primal-engine 可执行：环境变量优先，再按同构构建布局
    /// 探测（客户端在 Darwin/PrimalCli/&lt;cfg&gt;，引擎在 Darwin/&lt;cfg&gt;）。</summary>
    private static string? LocatePrimalEngine()
    {
        var env = Environment.GetEnvironmentVariable("PRIMAL_ENGINE_BIN");
        if (!string.IsNullOrEmpty(env) && File.Exists(env)) return env;

        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "DebugEditor", "primal-engine"),
            Path.Combine(AppContext.BaseDirectory, "primal-engine"),
            Path.Combine(AppContext.BaseDirectory, "..", "..", "DebugEditor", "primal-engine"),
        };
        foreach (var raw in candidates)
        {
            var full = Path.GetFullPath(raw);
            if (File.Exists(full)) return full;
        }
        return null;
    }

    private static int RouteOffline(string verb, System.Collections.Generic.List<string> restArgs)
    {
        // `primal engine info` 的双词形式在客户端侧收拢为子进程参数序列。
        var engine = LocatePrimalEngine();
        if (engine == null)
        {
            Console.Error.WriteLine("primal: 未找到 primal-engine（离线执行器）");
            Console.Error.WriteLine("  先构建: cmake --build build-wt --target PrimalEngineCli");
            Console.Error.WriteLine("  或用 PRIMAL_ENGINE_BIN 指定路径");
            return CliExitCodes.EditorNotRunning;
        }

        var childArgs = new System.Collections.Generic.List<string> { verb };
        foreach (var arg in restArgs) childArgs.Add(arg);
        Console.Error.WriteLine($"primal: 离线通道 → {engine}");
        var (jsonLine, exitCode) = RunOffline(engine, childArgs);
        if (jsonLine != null)
            Console.Out.WriteLine(JsonSerializer.Serialize(
                JsonSerializer.Deserialize<JsonElement>(jsonLine), PrettyOptions));
        return exitCode;
    }

    /// <summary>
    /// 运行 primal-engine 子进程：stderr 透传（日志可见），stdout 捕获并提取
    /// 响应 JSON 行（核心层尚有裸流走 stdout，见轨道 C）。
    /// 返回 (JSON 行或 null, 子进程退出码)。
    /// </summary>
    private static (string? JsonLine, int ExitCode) RunOffline(
        string engine, System.Collections.Generic.List<string> args)
    {
        var psi = new System.Diagnostics.ProcessStartInfo
        {
            FileName = engine,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = false,
        };
        foreach (var arg in args) psi.ArgumentList.Add(arg);

        using var process = System.Diagnostics.Process.Start(psi);
        if (process == null) return (null, CliExitCodes.EngineError);
        var stdout = process.StandardOutput.ReadToEnd();
        process.WaitForExit();

        var jsonLine = stdout.Split('\n')
            .LastOrDefault(line => line.StartsWith('{') && line.TrimEnd().EndsWith('}'))
            ?.TrimEnd();
        return (jsonLine, process.ExitCode);
    }

    // === test.run 的离线渲染基线门禁（§8/§10 Phase 2） ===

    private static int TestRender(System.Collections.Generic.List<string> restArgs)
    {
        string? baseline = null, heatmap = null;
        bool update = false;
        int threshold = ImageDiff.DefaultThreshold;
        var renderArgs = new System.Collections.Generic.List<string>();

        for (int i = 0; i < restArgs.Count; i++)
        {
            switch (restArgs[i])
            {
                case "--baseline" when i + 1 < restArgs.Count: baseline = restArgs[++i]; break;
                case "--heatmap" when i + 1 < restArgs.Count: heatmap = restArgs[++i]; break;
                case "--update-baseline": update = true; break;
                case "--threshold" when i + 1 < restArgs.Count && int.TryParse(restArgs[++i], out var t): threshold = t; break;
                default: renderArgs.Add(restArgs[i]); break;
            }
        }

        // 渲染到临时文件（离线 render 是确定性的——同机同驱动逐像素一致）
        var actual = Path.Combine(Path.GetTempPath(), $"primal-test-render-{Guid.NewGuid():N}.png");
        renderArgs.AddRange(new[] { "--out", actual });
        if (!renderArgs.Contains("--width")) renderArgs.AddRange(new[] { "--width", "320" });
        if (!renderArgs.Contains("--height")) renderArgs.AddRange(new[] { "--height", "180" });
        if (!renderArgs.Contains("--frames")) renderArgs.AddRange(new[] { "--frames", "3" });

        var engine = LocatePrimalEngine();
        if (engine == null)
        {
            Console.Error.WriteLine("primal: 未找到 primal-engine（离线执行器）");
            return CliExitCodes.EditorNotRunning;
        }

        var childArgs = new System.Collections.Generic.List<string> { "render" };
        foreach (var arg in renderArgs) childArgs.Add(arg);
        Console.Error.WriteLine("primal: test render — 离屏渲染基准帧…");
        var (jsonLine, exitCode) = RunOffline(engine, childArgs);
        if (jsonLine == null || exitCode != 0)
        {
            Console.Error.WriteLine($"primal: render 失败 (exit {exitCode})");
            return CliExitCodes.EngineError;
        }

        // baseline 默认落仓库 Testing/Baselines（cwd=仓库根的 CI 约定）
        baseline ??= Path.Combine("Testing", "Baselines", "offline-render-320x180.png");

        if (update || !File.Exists(baseline))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(baseline))!);
            File.Copy(actual, baseline, overwrite: true);
            Console.Out.WriteLine(JsonSerializer.Serialize(new
            {
                ok = true,
                data = new
                {
                    verdict = "baseline-created",
                    baseline = Path.GetFullPath(baseline),
                    actual,
                },
            }, PrettyOptions));
            Console.Error.WriteLine(update
                ? "primal: 基线已更新（--update-baseline）"
                : "primal: 基线不存在，已用本次渲染创建。提交该基线后 CI 即开始门禁。");
            return CliExitCodes.Success;
        }

        var diff = ImageDiff.Compare(actual, baseline, heatmap, threshold);
        // 尺寸不匹配本身就是结果（基线过期或渲染配置漂移）——必须失败，
        // 否则 ChangedPixelRatio=0 的早退分支会把尺寸漂移静默放行。
        var verdict = !diff.SameSize ? "different-size"
            : diff.Identical ? "identical"
            : diff.ChangedPixelRatio == 0 ? "within-threshold" : "different";
        var passed = verdict is "identical" or "within-threshold";
        Console.Out.WriteLine(JsonSerializer.Serialize(new
        {
            ok = passed,
            data = new
            {
                verdict,
                baseline = Path.GetFullPath(baseline),
                actual,
                meanAbsDelta = diff.MeanAbsDelta,
                maxAbsDelta = diff.MaxAbsDelta,
                changedPixelRatio = diff.ChangedPixelRatio,
                changedPixelCount = diff.ChangedPixelCount,
                threshold,
                heatmap,
            },
        }, PrettyOptions));

        try { File.Delete(actual); } catch { }

        if (!passed)
        {
            Console.Error.WriteLine("primal: 渲染结果与基线不一致——若是预期改动，用 --update-baseline 更新基线并在 PR 说明");
            return CliExitCodes.EngineError;
        }
        return CliExitCodes.Success;
    }

    /// M2 T3：离线物理门禁 —— `primal test physics`。
    /// primal-engine physics-sim 离屏仿真（地面+动态盒坠落，PipelineTick 驱动），
    /// C++ 侧已断言终态 y∈[0.44,0.56]（退出码承载），本层再做 PNG 基线比对。
    private static int TestPhysics(System.Collections.Generic.List<string> restArgs)
    {
        string? baseline = null, heatmap = null;
        bool update = false;
        int threshold = ImageDiff.DefaultThreshold;
        var simArgs = new System.Collections.Generic.List<string>();

        for (int i = 0; i < restArgs.Count; i++)
        {
            switch (restArgs[i])
            {
                case "--baseline" when i + 1 < restArgs.Count: baseline = restArgs[++i]; break;
                case "--heatmap" when i + 1 < restArgs.Count: heatmap = restArgs[++i]; break;
                case "--update-baseline": update = true; break;
                case "--threshold" when i + 1 < restArgs.Count && int.TryParse(restArgs[++i], out var t): threshold = t; break;
                default: simArgs.Add(restArgs[i]); break;
            }
        }

        var actual = Path.Combine(Path.GetTempPath(), $"primal-test-physics-{Guid.NewGuid():N}.png");
        simArgs.AddRange(new[] { "--out", actual });
        if (!simArgs.Contains("--width")) simArgs.AddRange(new[] { "--width", "320" });
        if (!simArgs.Contains("--height")) simArgs.AddRange(new[] { "--height", "180" });
        if (!simArgs.Contains("--frames")) simArgs.AddRange(new[] { "--frames", "120" });

        var engine = LocatePrimalEngine();
        if (engine == null)
        {
            Console.Error.WriteLine("primal: 未找到 primal-engine（离线执行器）");
            return CliExitCodes.EditorNotRunning;
        }

        var childArgs = new System.Collections.Generic.List<string> { "physics-sim" };
        foreach (var arg in simArgs) childArgs.Add(arg);
        Console.Error.WriteLine("primal: test physics — 离屏物理仿真（120 帧坠落）…");
        var (jsonLine, exitCode) = RunOffline(engine, childArgs);
        if (jsonLine == null || exitCode != 0)
        {
            Console.Error.WriteLine($"primal: physics-sim 失败 (exit {exitCode})——物理数值断言未通过或引擎错误");
            return CliExitCodes.EngineError;
        }

        baseline ??= Path.Combine("Testing", "Baselines", "physics-sim-320x180.png");

        if (update || !File.Exists(baseline))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(baseline))!);
            File.Copy(actual, baseline, overwrite: true);
            Console.Out.WriteLine(JsonSerializer.Serialize(new
            {
                ok = true,
                data = new
                {
                    verdict = "baseline-created",
                    baseline = Path.GetFullPath(baseline),
                    actual,
                },
            }, PrettyOptions));
            Console.Error.WriteLine(update
                ? "primal: 基线已更新（--update-baseline）"
                : "primal: 基线不存在，已用本次仿真创建。提交该基线后 CI 即开始门禁。");
            return CliExitCodes.Success;
        }

        var diff = ImageDiff.Compare(actual, baseline, heatmap, threshold);
        var verdict = !diff.SameSize ? "different-size"
            : diff.Identical ? "identical"
            : diff.ChangedPixelRatio == 0 ? "within-threshold" : "different";
        var passed = verdict is "identical" or "within-threshold";
        Console.Out.WriteLine(JsonSerializer.Serialize(new
        {
            ok = passed,
            data = new
            {
                verdict,
                baseline = Path.GetFullPath(baseline),
                actual,
                meanAbsDelta = diff.MeanAbsDelta,
                maxAbsDelta = diff.MaxAbsDelta,
                changedPixelRatio = diff.ChangedPixelRatio,
                changedPixelCount = diff.ChangedPixelCount,
                threshold,
                heatmap,
            },
        }, PrettyOptions));

        try { File.Delete(actual); } catch { }

        if (!passed)
        {
            Console.Error.WriteLine("primal: 物理仿真画面与基线不一致——若是预期改动，用 --update-baseline 更新基线并在 PR 说明");
            return CliExitCodes.EngineError;
        }
        return CliExitCodes.Success;
    }

    // === schema 双表聚合（§3.2：一个逻辑命名空间、两个执行器、一张聚合 schema） ===

    private static async Task<int> AggregateSchema()
    {
        // editor 半张表：活编辑器（尽力而为，未运行置 null）
        CliResponse? editorResponse = null;
        try
        {
            var (resp, _) = await CliClient.InvokeOnceAsync("editor.schema", null, 3_000).ConfigureAwait(false);
            editorResponse = resp;
        }
        catch { /* editor 不可达 */ }

        // offline 半张表：primal-engine（缺失置 null）
        object? offline = null;
        var engine = LocatePrimalEngine();
        if (engine != null)
        {
            try
            {
                var psi = new System.Diagnostics.ProcessStartInfo
                {
                    FileName = engine,
                    ArgumentList = { "schema" },
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                };
                using var process = System.Diagnostics.Process.Start(psi);
                var stdout = process?.StandardOutput.ReadToEnd();
                process?.WaitForExit();
                if (!string.IsNullOrEmpty(stdout))
                {
                    using var doc = JsonDocument.Parse(stdout);
                    // 子进程返回 CliResponse 包装，聚合只取其中的表
                    if (doc.RootElement.TryGetProperty("data", out var data))
                        offline = data.Clone();
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"primal: primal-engine schema 失败: {ex.Message}");
            }
        }

        var aggregated = new System.Collections.Generic.Dictionary<string, object?>
        {
            ["protocol"] = "primal.cli.v1",
            ["editor"] = editorResponse is { Ok: true, Data: not null } ? editorResponse.Data : null,
            ["offline"] = offline,
        };
        Console.Out.WriteLine(JsonSerializer.Serialize(aggregated, PrettyOptions));

        if (editorResponse is not { Ok: true })
            Console.Error.WriteLine("primal: editor 表不可用（编辑器未运行？）——只输出 offline 表");
        if (offline == null)
            Console.Error.WriteLine("primal: offline 表不可用（primal-engine 未找到？）");

        return CliExitCodes.Success;
    }

    private static int Discover()
    {
        var sockets = CliClient.DiscoverSocketPaths();
        Console.Out.WriteLine(JsonSerializer.Serialize(new
        {
            sockets,
            count = sockets.Count,
        }, PrettyOptions));
        Console.Error.WriteLine(sockets.Count == 0
            ? "primal: 没有发现编辑器命令通道 (socket 位于 ~/.primal/editor-<pid>.sock)"
            : $"primal: {sockets.Count} 个候选 socket（需连接验证活跃性）");
        return CliExitCodes.Success;
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine("""
            primal — Primal Engine CLI / Agent 客户端 (Phase 0)

            用法:
              primal <verb> ['{"param":value}'] [选项]
              primal discover          列出候选 socket
              primal selftest          框架自测（无需编辑器）
              primal schema            等价于 `primal editor.schema`

            verb 示例:
              editor.ping              编辑器存活/版本
              editor.schema            全部命令的 JSON schema
              scene.tree               当前场景实体树
              editor.screenshot        视口截图（params: {"path":"out.png"}）

            选项:
              --socket <path>   显式指定命令 socket
              --timeout <ms>    响应超时（默认 30000）
              --wait <sec>      等待编辑器出现（CI 冒烟用）

            输出契约: stdout=JSON，stderr=日志；退出码 0/2/3/4/5
            """);
    }
}
