using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.DeviceData;

namespace PrimalEditor_Avalonia.EngineeringSim;

/// <summary>单个结果字段值（engineering-simulation S1，2026-10-15）。</summary>
public sealed class SimFieldValue
{
    public double Value { get; set; }
    /// <summary>单位（空串 = 无量纲；"unknown" 保留——不猜不拒）。</summary>
    public string Unit { get; set; } = "";
    /// <summary>可选时间戳（瞬态结果；null = 稳态）。</summary>
    public DateTime? TimeUtc { get; set; }
}

/// <summary>外部求解结果数据集：node → field → 值。schemaVersion 拒未知版本。</summary>
public sealed class SimResultDataset
{
    public const int CurrentSchemaVersion = 1;

    [JsonPropertyName("schemaVersion")] public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    [JsonPropertyName("solverName")] public string SolverName { get; set; } = "";
    [JsonPropertyName("solverVersion")] public string SolverVersion { get; set; } = "";
    [JsonPropertyName("runId")] public string RunId { get; set; } = "";
    [JsonPropertyName("importedAtUtc")] public DateTime ImportedAtUtc { get; set; }

    [JsonPropertyName("fields")]
    public Dictionary<string, Dictionary<string, SimFieldValue>> Fields { get; set; } = new();

    /// <summary>单位一致性检查：同名字段跨节点单位不一致 → 报告（不静默、不代改）。</summary>
    public List<string> UnitInconsistencies()
    {
        var result = new List<string>();
        var byField = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        foreach (var (node, fields) in Fields)
        {
            foreach (var (field, v) in fields)
            {
                if (!byField.TryGetValue(field, out var units))
                {
                    units = new HashSet<string>(StringComparer.Ordinal);
                    byField[field] = units;
                }
                units.Add(v.Unit);
            }
        }
        foreach (var (field, units) in byField)
        {
            if (units.Count > 1)
                result.Add($"字段 '{field}' 跨节点单位不一致: {string.Join(", ", units.OrderBy(u => u, StringComparer.Ordinal))}");
        }
        return result;
    }

    /// <summary>读取并按需单位转换（复用 DeviceData 的仿射表）；未知对 → false。</summary>
    public bool TryRead(string node, string field, string targetUnit, out double value)
    {
        value = 0;
        if (!Fields.TryGetValue(node, out var fields) || !fields.TryGetValue(field, out var v))
            return false;
        if (!UnitConversions.TryConvert(v.Unit == "" ? targetUnit : v.Unit, targetUnit, v.Value, out var converted))
            return false;
        value = converted;
        return true;
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    /// <summary>坏 JSON / 未知 schemaVersion → (null, 错误消息)；不静默。</summary>
    public static (SimResultDataset? dataset, string? error) Load(string json)
    {
        try
        {
            var doc = JsonSerializer.Deserialize<SimResultDataset>(json, JsonOptions);
            if (doc == null) return (null, "数据集反序列化为 null");
            if (doc.SchemaVersion > CurrentSchemaVersion)
                return (null, $"schemaVersion {doc.SchemaVersion} 超过本版本 {CurrentSchemaVersion}——不静默覆写");
            return (doc, null);
        }
        catch (JsonException ex)
        {
            return (null, $"数据集 JSON 非法: {ex.Message}");
        }
    }
}

/// <summary>
/// 求解器 CSV 适配器（S1 的"外部接入优先"最小面）：列头固定
/// node,field,value,unit[,time_utc(ISO-8601)]；'#' 开头为注释行。
/// 类型化错误：缺列头/非数值/空 node——逐行报错不中断整体（错误清单返回）。
/// </summary>
public static class SolverCsvAdapter
{
    public static (SimResultDataset? dataset, List<string> errors) Parse(
        string csvText, string solverName, string solverVersion, string runId)
    {
        var errors = new List<string>();
        var dataset = new SimResultDataset
        {
            SolverName = solverName,
            SolverVersion = solverVersion,
            RunId = runId,
            ImportedAtUtc = DateTime.UtcNow,
        };
        var lines = csvText.Split('\n');
        string[]? header = null;
        int lineNo = 0;
        foreach (var raw in lines)
        {
            ++lineNo;
            var line = raw.Trim().TrimEnd('\r');
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var cols = line.Split(',');
            if (header == null)
            {
                header = cols.Select(c => c.Trim()).ToArray();
                foreach (var required in new[] { "node", "field", "value" })
                {
                    if (!header.Contains(required, StringComparer.OrdinalIgnoreCase))
                    {
                        errors.Add($"列头缺少 '{required}'（第 {lineNo} 行）");
                        return (null, errors);   // 结构性错误：整体拒绝
                    }
                }
                continue;
            }
            if (cols.Length < header.Length)
            {
                errors.Add($"第 {lineNo} 行列数 {cols.Length} < 列头 {header.Length}——跳过");
                continue;
            }
            var node = cols[Array.IndexOf(header, "node")].Trim();
            var field = cols[Array.IndexOf(header, "field")].Trim();
            var valueText = cols[Array.IndexOf(header, "value")].Trim();
            if (node.Length == 0 || field.Length == 0)
            {
                errors.Add($"第 {lineNo} 行 node/field 为空——跳过");
                continue;
            }
            if (!double.TryParse(valueText, NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
                || double.IsNaN(value) || double.IsInfinity(value))
            {
                errors.Add($"第 {lineNo} 行 value 非有限数值 '{valueText}'——跳过");
                continue;
            }
            var unit = header.Contains("unit", StringComparer.OrdinalIgnoreCase)
                ? cols[Array.IndexOf(header, "unit")].Trim()
                : "";
            DateTime? time = null;
            if (header.Contains("time_utc", StringComparer.OrdinalIgnoreCase))
            {
                var t = cols[Array.IndexOf(header, "time_utc")].Trim();
                if (DateTime.TryParse(t, CultureInfo.InvariantCulture,
                        DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var parsed))
                    time = parsed;
            }
            if (!dataset.Fields.TryGetValue(node, out var fields))
            {
                fields = new Dictionary<string, SimFieldValue>(StringComparer.Ordinal);
                dataset.Fields[node] = fields;
            }
            fields[field] = new SimFieldValue { Value = value, Unit = unit, TimeUtc = time };
        }
        if (header == null)
        {
            errors.Add("CSV 为空（无列头）");
            return (null, errors);
        }
        return (dataset, errors);
    }
}
