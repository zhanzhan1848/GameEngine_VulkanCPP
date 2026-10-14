using System;
using System.Collections.Generic;

namespace PrimalEditor_Avalonia.DeviceData;

/// <summary>数据质量（device-data S1）。时间语义：新鲜=Good、超窗=Uncertain、
/// 缺失=Unknown、被源标记坏=Bad——不静默把过期当好。</summary>
public enum DeviceQuality
{
    Good = 0,
    Uncertain = 1,
    Bad = 2,
    Unknown = 3,
}

/// <summary>一条设备采样（时间戳_utc + 设备/信号 + 值/单位/质量）。</summary>
public sealed class DeviceSample
{
    public DateTime TimestampUtc { get; init; } = DateTime.UtcNow;
    public string DeviceId { get; init; } = "";
    public string Signal { get; init; } = "";
    public double Value { get; init; }
    public string Unit { get; init; } = "";
    /// <summary>源侧质量（Bad = 源自报坏值）；消费侧新鲜度质量由 store 评。</summary>
    public DeviceQuality SourceQuality { get; init; } = DeviceQuality.Good;
}

/// <summary>信号定义（模拟源与映射共用）：单位、量程、波形。</summary>
public sealed class SignalDescriptor
{
    public string Signal { get; init; } = "";
    public string Unit { get; init; } = "";
    public double Min { get; init; } = 0;
    public double Max { get; init; } = 1;
    public SimWaveform Waveform { get; init; } = SimWaveform.Sine;
    /// <summary>周期（秒）；Sine/Square 用。</summary>
    public double PeriodSeconds { get; init; } = 60;
}

public enum SimWaveform { Sine, Square, Constant }

/// <summary>单位仿射转换（u_to = factor × u_from + offset）。
/// S1 内置常用工程对；未知对返回 false——不猜系数。</summary>
public static class UnitConversions
{
    private static readonly Dictionary<(string From, string To), (double Factor, double Offset)> Table =
        new()
        {
            [("degC", "degF")] = (1.8, 32.0),
            [("degF", "degC")] = (5.0 / 9.0, -32.0 * 5.0 / 9.0),
            [("degC", "K")] = (1.0, 273.15),
            [("K", "degC")] = (1.0, -273.15),
            [("m", "mm")] = (1000.0, 0.0),
            [("mm", "m")] = (0.001, 0.0),
            [("kPa", "Pa")] = (1000.0, 0.0),
            [("Pa", "kPa")] = (0.001, 0.0),
            [("bar", "kPa")] = (100.0, 0.0),
            [("kPa", "bar")] = (0.01, 0.0),
        };

    public static bool TryConvert(string from, string to, double value, out double result)
    {
        if (from == to) { result = value; return true; }
        if (Table.TryGetValue((from, to), out var conv))
        {
            result = conv.Factor * value + conv.Offset;
            return true;
        }
        result = 0;
        return false;
    }

    /// <summary>已注册的单位对（诊断/CLI 展示）。</summary>
    public static IEnumerable<(string From, string To)> RegisteredPairs
    {
        get { foreach (var k in Table.Keys) yield return (k.From, k.To); }
    }
}

/// <summary>信号 → 实体属性的绑定记录（device-data S1；应用走宿主注入的
/// setter——不直接接触引擎，与 SequenceDriver 的属性应用同边界）。</summary>
public sealed class DeviceBinding
{
    public string DeviceId { get; init; } = "";
    public string Signal { get; init; } = "";
    public Guid EntityGuid { get; init; }
    public string Component { get; init; } = "";
    public string Property { get; init; } = "";
    /// <summary>目标单位（null = 原样）。</summary>
    public string? TargetUnit { get; init; }
    /// <summary>死区（目标单位）：变化不超过死区不重复应用。</summary>
    public double Deadband { get; init; }
    /// <summary>true = Uncertain（过期但存在）也应用；Bad/Unknown 永不应用。</summary>
    public bool ApplyWhenUncertain { get; init; }
}
