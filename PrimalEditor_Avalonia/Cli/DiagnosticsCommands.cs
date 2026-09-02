using System;
using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 日志消费 verbs（CLI 规划 §11.4 L3）：让 agent 不翻全文、按级别/类别拉取
/// 引擎黑匣子（最近 2048 条，Release 常开）。数据源是 EngineDLL 的
/// CopyRecentLogs 导出——与 stderr/FileSink/事件表同源。
/// </summary>
public sealed class DiagnosticsCommands
{
    private static readonly string[] LevelNames = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

    private static int LevelIndex(string? level)
    {
        if (string.IsNullOrWhiteSpace(level)) return 2; // 默认 INFO 起
        var match = LevelNames.Select((n, i) => (n, i)).FirstOrDefault(t => t.n.Equals(level, StringComparison.OrdinalIgnoreCase));
        if (match.n != null) return match.i;
        throw CliError.InvalidParams($"未知级别 '{level}'",
            "有效取值: " + string.Join("/", LevelNames));
    }

    [CliCommand("editor.logs",
        Description = "拉取引擎黑匣子日志（最近 2048 条环形缓冲，含引擎任意线程的 PRIMAL 日志）。" +
                      "tail 上限 1000（默认 50，取最新）；level 为最低级别阈值" +
                      "（TRACE/DEBUG/INFO/WARN/ERROR/FATAL，默认 INFO）；category 可选精确类别过滤" +
                      "（如 RHI/Asset/Render）。纯读操作。引擎未加载返回 NOT_READY。",
        Example = """{"tail":200,"level":"warn","category":"RHI"}""")]
    public object Logs(int tail = 50, string? level = null, string? category = null)
    {
        if (tail < 1) throw CliError.InvalidParams("tail 必须 ≥ 1");
        if (tail > 1000) throw CliError.InvalidParams("tail 上限 1000", "缩小 level/category 范围代替");
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载", "用 editor.ping 查看 engineLoaded");

        var minLevel = LevelIndex(level);
        var text = NativeEngineBridge.CopyRecentLogs();
        if (text == null)
            throw new CliError(CliErrorCodes.EngineError, "黑匣子读取失败（导出缺失或 marshaling 错误）",
                "查看编辑器 stderr 的 [NativeEngineBridge] 诊断行");
        if (text.Length == 0)
            return new { count = 0, entries = Array.Empty<object>(), hint = "黑匣子为空（引擎刚启动）" };

        var entries = new List<object>();
        var totalLines = 0;
        foreach (var rawLine in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            // 行格式（CopyRecentLogs 契约）："[HH:MM:SS.mmm][LEVEL][category] message"
            var line = rawLine.TrimEnd();
            if (line.Length < 8 || line[0] != '[') continue;
            var close1 = line.IndexOf(']');
            var open2 = line.IndexOf('[', close1);
            var close2 = line.IndexOf(']', open2);
            var open3 = line.IndexOf('[', close2);
            var close3 = line.IndexOf(']', open3);
            if (close1 < 0 || open2 < 0 || close2 < 0 || open3 < 0 || close3 < 0) continue;
            var time = line[1..close1];
            var levelName = line[(open2 + 1)..close2];
            var cat = line[(open3 + 1)..close3];
            var message = line[(close3 + 1)..].TrimStart();

            var levelIdx = Array.FindIndex(LevelNames, n => n == levelName);
            if (levelIdx < 0) continue;
            if (levelIdx < minLevel) continue;
            totalLines++;
            if (!string.IsNullOrEmpty(category) && !cat.Equals(category, StringComparison.Ordinal)) continue;
            entries.Add(new { time, level = levelName, category = cat, message });
        }

        var selected = entries.Skip(Math.Max(0, entries.Count - tail)).ToArray();
        return new
        {
            count = selected.Length,
            matchedTotal = entries.Count,
            bufferTotal = totalLines,
            entries = selected,
        };
    }
    [CliCommand("editor.log-filter",
        Description = "设置引擎日志类别白名单（对 stderr/文件/黑匣子/事件表全部 sink 生效）。" +
                      "categories 逗号分隔（如 \"RHI,Asset,Render\"）；null/空串 = 不过滤（全类别）。" +
                      "与 editor.logs 的只读 category 过滤不同，本命令改变哪些类别**进入**日志管道。" +
                      "返回生效后的白名单。",
        Example = """{"categories":"RHI,Render"}""")]
    public object SetLogFilter(string? categories = null)
    {
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载", "用 editor.ping 查看 engineLoaded");
        NativeEngineBridge.SetCategoryFilter(string.IsNullOrWhiteSpace(categories) ? null : categories.Trim());
        return new
        {
            filter = string.IsNullOrWhiteSpace(categories) ? null : categories.Trim(),
            filtering = !string.IsNullOrWhiteSpace(categories),
            hint = "白名单持久到进程退出；editor.logs 不受影响（读取完整环形缓冲不受级别影响，受级别阈值影响）",
        };
    }
}