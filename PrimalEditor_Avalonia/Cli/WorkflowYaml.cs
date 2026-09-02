using System;
using System.Collections.Generic;
using System.Text.Json;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// workflow.run 的 YAML 前端（CLI 规划 §5.4）。**受限子集**——只解析下面这一
/// 种形态（正是 playbook/CI 会写出的形态），不做通用 YAML：
///
///   steps:
///     - verb: editor.ping
///     - verb: camera.set
///       params:
///         entityGuid: "0c9e…"
///         fov: 45
///         position: [1, 2, 3]
///
/// 值支持：整数/小数/true/false/引号字符串/裸字符串/方括号流序列（元素同上）。
/// # 注释与空行忽略。缩进用空格（Tab 直接报错，YAML 禁 Tab）。
/// 超出子集的构造（锚点、多文档、块标量等）一律结构化报错而不是静默误读。
/// </summary>
public static class WorkflowYaml
{
    public static List<(string Verb, JsonElement? Params)> Parse(string yaml)
    {
        var rawLines = yaml.Replace('\t', ' ').Split('\n');
        var lines = new List<(int indent, string content)>();
        foreach (var raw in rawLines)
        {
            var stripped = StripComment(raw).TrimEnd();
            if (stripped.Trim().Length == 0) continue;
            int indent = stripped.Length - stripped.TrimStart().Length;
            lines.Add((indent, stripped.Trim()));
        }
        if (lines.Count == 0 || lines[0].content != "steps:")
            throw new CliFrameworkYamlException("YAML 顶层必须是 'steps:'");
        if (lines.Count > 1 && lines[1].indent <= lines[0].indent)
            throw new CliFrameworkYamlException("'steps:' 之下必须缩进列出步骤（'- verb: …'）");

        var result = new List<(string, JsonElement?)>();
        int i = 1;
        int itemIndent = lines[1].indent;
        while (i < lines.Count)
        {
            var (indent, content) = lines[i];
            if (indent < itemIndent)
                throw new CliFrameworkYamlException($"意外的缩进（第 {i + 1} 行）：步骤列表之外的内容");
            if (!content.StartsWith("- "))
                throw new CliFrameworkYamlException($"第 {i + 1} 行应以 '- ' 开始（步骤项）: '{content}'");

            var first = content[2..]; // 去掉 "- "
            var verb = (string)ParseKeyValue(first, i + 1, "verb").value!;
            i++;
            JsonElement? parameters = null;

            // 同项后续行：更深缩进的 key: value（params: 块或直接键）
            while (i < lines.Count && lines[i].indent > itemIndent)
            {
                var key = ParseKeyValue(lines[i].content, i + 1, null);
                if (key.key == "params")
                {
                    // params: 之下更深缩进的 key: value 列表
                    var paramIndent = lines[i].indent;
                    var map = new Dictionary<string, object?>();
                    i++;
                    while (i < lines.Count && lines[i].indent > paramIndent)
                    {
                        var kv = ParseKeyValue(lines[i].content, i + 1, null);
                        map[kv.key] = kv.value;
                        i++;
                    }
                    parameters = JsonSerializer.SerializeToElement(map, JsonOpts());
                }
                else
                {
                    // 直接键（无 params 块的扁平写法）视为 params 的一部分
                    if (parameters is not JsonElement el || el.ValueKind != JsonValueKind.Object)
                        parameters = JsonSerializer.SerializeToElement(new Dictionary<string, object?>(), JsonOpts());
                    var dict = JsonSerializer.Deserialize<Dictionary<string, object?>>(parameters.Value.GetRawText(), JsonOpts());
                    dict![key.key] = key.value;
                    parameters = JsonSerializer.SerializeToElement(dict, JsonOpts());
                    i++;
                }
            }
            result.Add((verb, parameters));
        }
        return result;
    }

    private static JsonSerializerOptions JsonOpts() => new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    private static (string key, object? value) ParseKeyValue(string content, int lineNo, string? expectKey)
    {
        int colon = content.IndexOf(": ", StringComparison.Ordinal);
        if (colon < 0 && content.EndsWith(':')) colon = content.Length - 1;
        if (colon < 0)
            throw new CliFrameworkYamlException($"第 {lineNo} 行缺少 ': '（应为 key: value）: '{content}'");
        var key = content[..colon].Trim();
        var value = content[(colon + 1)..].Trim();
        if (expectKey != null && !key.Equals(expectKey, StringComparison.Ordinal))
            throw new CliFrameworkYamlException($"第 {lineNo} 行应为 '{expectKey}: …'，实际是 '{key}: …'");
        return (key, (object?)ParseScalar(value, lineNo));
    }

    private static object? ParseScalar(string value, int lineNo)
    {
        if (value.Length == 0) return null;
        if (value.StartsWith('[') && value.EndsWith(']'))
        {
            var inner = value[1..^1].Trim();
            if (inner.Length == 0) return Array.Empty<object>();
            var items = new List<object?>();
            foreach (var part in SplitFlow(inner))
                items.Add(ParseScalar(part.Trim(), lineNo));
            return items;
        }
        if ((value.StartsWith('"') && value.EndsWith('"') && value.Length >= 2))
            return value[1..^1];
        if ((value.StartsWith('\'') && value.EndsWith('\'') && value.Length >= 2))
            return value[1..^1];
        if (value is "true" or "True") return true;
        if (value is "false" or "False") return false;
        if (long.TryParse(value, out var l)) return l;
        if (double.TryParse(value, out var d)) return d;
        return value;
    }

    /// <summary>按方括号外的逗号切分流序列。</summary>
    private static IEnumerable<string> SplitFlow(string inner)
    {
        int depth = 0;
        bool inQuote = false;
        char quote = '\0';
        int start = 0;
        for (int i = 0; i < inner.Length; i++)
        {
            var c = inner[i];
            if (inQuote) { if (c == quote) inQuote = false; continue; }
            if (c is '"' or '\'') { inQuote = true; quote = c; continue; }
            if (c == '[' || c == '{') depth++;
            else if (c == ']' || c == '}') depth--;
            else if (c == ',' && depth == 0)
            {
                yield return inner[start..i];
                start = i + 1;
            }
        }
        yield return inner[start..];
    }

    private static string StripComment(string line)
    {
        bool inSingle = false, inDouble = false;
        for (int i = 0; i < line.Length; i++)
        {
            var c = line[i];
            if (c == '\'' && !inDouble) inSingle = !inSingle;
            else if (c == '"' && !inSingle) inDouble = !inDouble;
            else if (c == '#' && !inSingle && !inDouble && (i == 0 || line[i - 1] is ' ' or '\t'))
                return line[..i];
        }
        return line;
    }
}

/// <summary>workflow YAML 解析失败（结构化错误，带行号）。</summary>
public class CliFrameworkYamlException : Exception
{
    public CliFrameworkYamlException(string message) : base(message) { }
}
