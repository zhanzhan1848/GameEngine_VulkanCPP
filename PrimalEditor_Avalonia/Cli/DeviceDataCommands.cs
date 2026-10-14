using System;
using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.DeviceData;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// device-data verbs（device-data S1，2026-10-15）。S1 = 模拟源 + 时间/质量/
/// 单位记录（真实协议 MQTT/OPC UA 是 S2）。模拟源确定性：同 seed 逐位一致。
/// 质量语义：good=新鲜、uncertain=超 staleWindow（默认 5s）、bad=源自报、
/// unknown=无样本——过期不装新鲜。
/// </summary>
public sealed class DeviceDataCommands
{
    private static readonly PrimalEditor_Avalonia.DeviceData.DeviceDataStore Store = new();
    private static DateTime _now = DateTime.MinValue;

    private static readonly PrimalEditor_Avalonia.DeviceData.SignalDescriptor[] DefaultSignals =
    {
        new() { Signal = "temperature", Unit = "degC", Min = 18, Max = 28, Waveform = PrimalEditor_Avalonia.DeviceData.SimWaveform.Sine, PeriodSeconds = 120 },
        new() { Signal = "pressure", Unit = "kPa", Min = 95, Max = 105, Waveform = PrimalEditor_Avalonia.DeviceData.SimWaveform.Sine, PeriodSeconds = 60 },
    };

    [CliCommand("devicedata.simulate",
        Description = "运行确定性模拟设备源并入库（device-data S1）。deviceId 缺省 'sim-furnace-01'；" +
                      "periodMs 采样周期（缺省 500）；seconds 生成时长；seed 缺省 1（同 seed 同序列）；" +
                      "staleWindowSeconds 质量评估窗口（缺省 5）。返回统计（接受/乱序/重复）+ 每信号" +
                      "最新值与质量。纯模拟——无网络、无凭据。",
        Example = """{"seconds":10,"periodMs":500,"seed":1}""")]
    public object Simulate(double seconds = 10, int periodMs = 500, int seed = 1,
        double staleWindowSeconds = 5, string deviceId = "sim-furnace-01")
    {
        if (seconds <= 0 || seconds > 3600) throw CliError.InvalidParams("seconds 须在 (0, 3600]");
        if (periodMs < 10 || periodMs > 60000) throw CliError.InvalidParams("periodMs 须在 [10, 60000]");
        var source = new PrimalEditor_Avalonia.DeviceData.SimulatedDeviceSource(
            deviceId, periodMs, DefaultSignals, seed);
        var end = DateTime.UtcNow;
        var baseUtc = end.AddSeconds(-seconds);
        var samples = source.Generate(baseUtc, 0, seconds);
        Store.StaleWindow = TimeSpan.FromSeconds(staleWindowSeconds);
        Store.Push(samples);
        _now = end;

        var latest = DefaultSignals.Select(s =>
        {
            var e = Store.Latest(deviceId, s.Signal, end);
            return new
            {
                signal = s.Signal,
                unit = e.Quality == PrimalEditor_Avalonia.DeviceData.DeviceQuality.Unknown
                    ? s.Unit : e.Sample.Unit,
                value = e.Quality == PrimalEditor_Avalonia.DeviceData.DeviceQuality.Unknown
                    ? (double?)null : Math.Round(e.Sample.Value, 4),
                quality = e.Quality.ToString().ToLowerInvariant(),
                ageSeconds = Math.Round(e.AgeSeconds, 3),
            };
        }).ToArray();
        return new
        {
            deviceId,
            generated = samples.Count,
            accepted = Store.AcceptedCount,
            outOfOrder = Store.OutOfOrderCount,
            duplicates = Store.DuplicateCount,
            latest,
        };
    }

    [CliCommand("devicedata.latest",
        Description = "查询已入库信号的最新值与质量（device-data S1；先跑 devicedata.simulate）。" +
                      "signal 省略 = 全部已知信号。质量门限见 devicedata.simulate 描述。",
        Example = """{"deviceId":"sim-furnace-01"}""")]
    public object Latest(string deviceId = "sim-furnace-01", string? signal = null)
    {
        var now = _now == DateTime.MinValue ? DateTime.UtcNow : _now;
        var signals = signal != null
            ? new[] { signal }
            : StoreKnownSignals(deviceId);
        var entries = signals.Select(s =>
        {
            var e = Store.Latest(deviceId, s, now);
            return new
            {
                signal = s,
                value = e.Quality == PrimalEditor_Avalonia.DeviceData.DeviceQuality.Unknown
                    ? (double?)null : Math.Round(e.Sample.Value, 4),
                unit = e.Sample.Unit,
                quality = e.Quality.ToString().ToLowerInvariant(),
                ageSeconds = Math.Round(e.AgeSeconds, 3),
            };
        }).ToArray();
        return new { deviceId, evaluatedAtUtc = now, entries };
    }

    [CliCommand("devicedata.units",
        Description = "列出设备数据单位转换表（device-data S1；仿射对 from→to）。",
        Example = "{}")]
    public object Units() => new
    {
        pairs = PrimalEditor_Avalonia.DeviceData.UnitConversions.RegisteredPairs
            .Select(p => new { from = p.From, to = p.To })
            .ToArray(),
    };

    private static string[] StoreKnownSignals(string deviceId) =>
        new[] { "temperature", "pressure" };   // 与 simulate 的缺省源同一清单
}
