using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.DeviceData;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.DeviceDataNs;

/// <summary>
/// device-data S1 契约测试：模拟源确定性（同 seed 逐位一致/异 seed 不同）、
/// 参考序列数学（sine 起点=中点）、丢样间隙、store 质量（新鲜/过期/缺失/源 Bad）、
/// 乱序接受计数、重复丢弃计数、单位仿射转换（°C↔°F/K、未知对拒绝）、
/// 绑定应用（质量门限/死区/转换失败不静默）。
/// </summary>
public sealed class DeviceDataTests
{
    private static readonly SignalDescriptor Temp = new()
    {
        Signal = "temperature", Unit = "degC", Min = 18, Max = 28,
        Waveform = SimWaveform.Sine, PeriodSeconds = 120,
    };

    private static readonly DateTime Base = new(2026, 10, 15, 0, 0, 0, DateTimeKind.Utc);

    [Fact]
    public void Sim_SameSeed_IdenticalSeries()
    {
        var a = new SimulatedDeviceSource("d1", 500, new[] { Temp }, seed: 7);
        var b = new SimulatedDeviceSource("d1", 500, new[] { Temp }, seed: 7);
        var sa = a.Generate(Base, 0, 10);
        var sb = b.Generate(Base, 0, 10);
        Assert.Equal(sa.Count, sb.Count);
        for (int i = 0; i < sa.Count; ++i)
        {
            Assert.Equal(sa[i].TimestampUtc, sb[i].TimestampUtc);
            Assert.Equal(sa[i].Value, sb[i].Value, 12);   // 逐位一致
        }
    }

    [Fact]
    public void Sim_DifferentSeed_DifferentNoise()
    {
        var a = new SimulatedDeviceSource("d1", 500, new[] { Temp }, seed: 7, noiseFraction: 0.05);
        var b = new SimulatedDeviceSource("d1", 500, new[] { Temp }, seed: 8, noiseFraction: 0.05);
        var sa = a.Generate(Base, 0, 10);
        var sb = b.Generate(Base, 0, 10);
        Assert.NotEqual(sa.Select(s => s.Value).ToArray(), sb.Select(s => s.Value).ToArray());
    }

    [Fact]
    public void Sim_SineStartsAtMidpoint_NoNoise()
    {
        var src = new SimulatedDeviceSource("d1", 500, new[] { Temp }, seed: 1, noiseFraction: 0);
        Assert.Equal(23.0, src.ValueAt(Temp, 0), 10);         // sin(0)=0 → 中点 23
        var quarter = Temp.PeriodSeconds / 4;
        Assert.Equal(28.0, src.ValueAt(Temp, quarter), 10);   // 峰值 = Max
    }

    [Fact]
    public void Sim_Dropout_ProducesTimeGap()
    {
        var src = new SimulatedDeviceSource("d1", 1000, new[] { Temp }, seed: 1, dropEveryN: 3);
        var samples = src.Generate(Base, 0, 10);
        Assert.Equal(10 - 3, samples.Count);                   // 每 3 个丢 1 → 7 个
        var store = new DeviceDataStore();
        store.Push(samples);
        var (_, gaps) = store.Window("d1", "temperature", Base, Base.AddSeconds(10), 1.0);
        Assert.NotEmpty(gaps);                                 // 间隙 ≥ 一处
    }

    [Fact]
    public void Store_Quality_FreshStaleMissingBad()
    {
        var store = new DeviceDataStore { StaleWindow = TimeSpan.FromSeconds(5) };
        var now = Base.AddSeconds(100);
        store.Push(new[]
        {
            new DeviceSample { TimestampUtc = now - TimeSpan.FromSeconds(1), DeviceId = "d", Signal = "fresh", Value = 1, Unit = "degC" },
            new DeviceSample { TimestampUtc = now - TimeSpan.FromSeconds(30), DeviceId = "d", Signal = "stale", Value = 2, Unit = "degC" },
            new DeviceSample { TimestampUtc = now - TimeSpan.FromSeconds(1), DeviceId = "d", Signal = "bad", Value = 3, Unit = "degC", SourceQuality = DeviceQuality.Bad },
        });
        Assert.Equal(DeviceQuality.Good, store.Latest("d", "fresh", now).Quality);
        Assert.Equal(DeviceQuality.Uncertain, store.Latest("d", "stale", now).Quality);   // 过期≠新鲜
        Assert.Equal(DeviceQuality.Bad, store.Latest("d", "bad", now).Quality);
        Assert.Equal(DeviceQuality.Unknown, store.Latest("d", "missing", now).Quality);
    }

    [Fact]
    public void Store_OutOfOrderAccepted_DuplicatesDropped()
    {
        var store = new DeviceDataStore();
        var t1 = Base;
        var t2 = Base.AddSeconds(1);
        var t3 = Base.AddSeconds(2);
        store.Push(new[] { S("s", t3, 3), S("s", t2, 2) });    // 乱序进入
        store.Push(new[] { S("s", t3, 33) });                  // 重复时间戳
        store.Push(new[] { S("s", t1, 1) });                   // 又一个乱序
        Assert.Equal(2, store.OutOfOrderCount);
        Assert.Equal(1, store.DuplicateCount);
        Assert.Equal(3, store.Latest("d", "s", Base.AddSeconds(5)).Sample.Value, 6);   // latest 仍 3

        static DeviceSample S(string signal, DateTime t, double v) =>
            new() { TimestampUtc = t, DeviceId = "d", Signal = signal, Value = v, Unit = "degC" };
    }

    [Fact]
    public void Units_AffineConversions_ExactAndUnknownRejected()
    {
        Assert.True(UnitConversions.TryConvert("degC", "degF", 100, out var f));
        Assert.Equal(212.0, f, 10);
        Assert.True(UnitConversions.TryConvert("degF", "degC", 32, out var c));
        Assert.Equal(0.0, c, 10);
        Assert.True(UnitConversions.TryConvert("degC", "K", 0, out var k));
        Assert.Equal(273.15, k, 10);
        Assert.True(UnitConversions.TryConvert("m", "mm", 1.5, out var mm));
        Assert.Equal(1500.0, mm, 10);
        Assert.False(UnitConversions.TryConvert("degC", "kPa", 20, out _));   // 未知对不猜
    }

    [Fact]
    public void Bindings_QualityGate_Deadband_AndConversionError()
    {
        var store = new DeviceDataStore { StaleWindow = TimeSpan.FromSeconds(5) };
        var now = Base.AddSeconds(10);
        store.Push(new[]
        {
            new DeviceSample { TimestampUtc = now, DeviceId = "d", Signal = "temp", Value = 20, Unit = "degC" },
            new DeviceSample { TimestampUtc = now - TimeSpan.FromSeconds(99), DeviceId = "d", Signal = "old", Value = 1, Unit = "degC" },
        });
        var applied = new Dictionary<string, double>();
        bool Setter(DeviceBinding b, double v)
        {
            if (applied.TryGetValue(b.Property, out var prev) && Math.Abs(prev - v) < 0.5)
                return false;   // 死区由调用方实现（S1 契约：setter 决定接受与否）
            applied[b.Property] = v;
            return true;
        }
        var bindings = new[]
        {
            new DeviceBinding { DeviceId = "d", Signal = "temp", EntityGuid = Guid.NewGuid(), Component = "light", Property = "intensity", TargetUnit = "degF" },
            new DeviceBinding { DeviceId = "d", Signal = "old", EntityGuid = Guid.NewGuid(), Component = "x", Property = "oldProp" },
            new DeviceBinding { DeviceId = "d", Signal = "temp", EntityGuid = Guid.NewGuid(), Component = "y", Property = "badUnit", TargetUnit = "kPa" },
        };
        var results = DeviceBindingApply.ApplyLatest(store, bindings, now, Setter);
        Assert.True(results[0].Applied);
        Assert.Equal(68.0, results[0].Value!.Value, 10);           // 20°C → 68°F
        Assert.False(results[1].Applied);                          // 过期未开门限
        Assert.Null(results[2].Value);                             // 转换失败不静默
        Assert.Contains("单位转换不可用", results[2].Error);
    }
}

