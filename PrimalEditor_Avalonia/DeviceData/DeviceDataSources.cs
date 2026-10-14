using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.DeviceData;

/// <summary>
/// 确定性模拟设备源（device-data S1）。同 seed 同序列（逐位一致——参考序列
/// 验收的根基）；波形 = 区间 [Min,Max] 内的中点±幅度：Sine/Square/Constant，
/// 叠加 seed 驱动的伪随机噪声（LCG——不依赖框架 Random 的实现漂移）。
/// dropoutEveryN：每 N 个采样点跳过一个（确定性断流——缺值/间隙验收用）。
/// </summary>
public sealed class SimulatedDeviceSource
{
    private readonly List<SignalDescriptor> _signals;
    private readonly double _noiseAmplitude;   // 量程的比例
    private readonly int _dropEveryN;          // 0 = 不丢样
    private ulong _lcg;
    private int _emitted;

    public string DeviceId { get; }
    public int PeriodMilliseconds { get; }

    public SimulatedDeviceSource(string deviceId, int periodMilliseconds,
        IReadOnlyList<SignalDescriptor> signals, int seed = 1,
        double noiseFraction = 0.02, int dropEveryN = 0)
    {
        DeviceId = deviceId;
        PeriodMilliseconds = periodMilliseconds;
        _signals = signals.ToList();
        _lcg = 0x9E3779B97F4A7C15ul ^ (ulong)(uint)seed;
        _noiseAmplitude = noiseFraction;
        _dropEveryN = dropEveryN;
    }

    /// <summary>生成 [startSeconds, startSeconds + seconds) 内的采样。
    /// 时间戳 = 基准 + k×周期；丢样点整段跳过（时间戳仍前进——产生间隙）。</summary>
    public List<DeviceSample> Generate(DateTime baseUtc, double startSeconds, double seconds)
    {
        var result = new List<DeviceSample>();
        var period = PeriodMilliseconds / 1000.0;
        var firstIndex = (int)Math.Floor(startSeconds / period);
        var count = (int)Math.Ceiling(seconds / period);
        for (int k = 0; k < count; ++k)
        {
            var index = firstIndex + k;
            if (_dropEveryN > 0 && index % _dropEveryN == _dropEveryN - 1) continue;
            var t = index * period;
            foreach (var s in _signals)
            {
                result.Add(new DeviceSample
                {
                    TimestampUtc = baseUtc.AddSeconds(t),
                    DeviceId = DeviceId,
                    Signal = s.Signal,
                    Value = ValueAt(s, t),
                    Unit = s.Unit,
                    SourceQuality = DeviceQuality.Good,
                });
            }
        }
        return result;
    }

    /// <summary>单信号单时刻取值（纯函数——参考序列断言用）。</summary>
    public double ValueAt(SignalDescriptor s, double t)
    {
        var mid = (s.Min + s.Max) / 2;
        var amp = (s.Max - s.Min) / 2;
        var phase = s.PeriodSeconds > 0 ? t / s.PeriodSeconds : 0;
        var value = s.Waveform switch
        {
            SimWaveform.Sine => mid + amp * Math.Sin(2 * Math.PI * phase),
            SimWaveform.Square => mid + amp * (Math.Floor(phase * 2) % 2 == 0 ? 1 : -1),
            _ => mid,
        };
        if (_noiseAmplitude > 0)
        {
            var noise = NextNoise();
            value += (s.Max - s.Min) * _noiseAmplitude * noise;
        }
        return value;
    }

    /// <summary>[-1,1) 均匀噪声（LCG）——顺序敏感，保证确定性。</summary>
    private double NextNoise()
    {
        _lcg = _lcg * 6364136223846793005ul + 1442695040888963407ul;
        var bits = (uint)(_lcg >> 33);
        return bits / 2147483648.0 - 1.0;
    }
}

/// <summary>
/// 设备数据存储（device-data S1）：latest + 环形历史 + 消费侧质量评估。
/// 乱序：接受并计数（时间戳排序展示）；重复（同设备/信号/时间戳）：丢弃并计数。
/// 质量规则：Good=新鲜且源 Good；Uncertain=超 staleWindow 或源 Uncertain；
/// Bad=源 Bad；Unknown=无样本。
/// </summary>
public sealed class DeviceDataStore
{
    public sealed class Entry
    {
        public DeviceSample Sample = new();
        public DeviceQuality Quality = DeviceQuality.Unknown;
        public double AgeSeconds;
    }

    private readonly Dictionary<(string, string), DeviceSample> _latest = new();
    private readonly Dictionary<(string, string), List<DeviceSample>> _history = new();
    public TimeSpan StaleWindow { get; set; } = TimeSpan.FromSeconds(5);
    public int HistoryLimit { get; set; } = 4096;
    public long OutOfOrderCount { get; private set; }
    public long DuplicateCount { get; private set; }
    public long AcceptedCount { get; private set; }

    public void Push(IEnumerable<DeviceSample> samples)
    {
        foreach (var s in samples)
        {
            var key = (s.DeviceId, s.Signal);
            if (!_history.TryGetValue(key, out var list))
            {
                list = new List<DeviceSample>();
                _history[key] = list;
                _latest[key] = s;
                list.Add(s);
                ++AcceptedCount;
                continue;
            }
            // 重复：同时间戳已有样本 → 丢弃（首个胜出，可追溯）
            if (list.Any(e => e.TimestampUtc == s.TimestampUtc))
            {
                ++DuplicateCount;
                continue;
            }
            // 乱序：早于已有最新样本 → 接受进历史但不更新 latest
            if (s.TimestampUtc < _latest[key].TimestampUtc)
            {
                ++OutOfOrderCount;
                list.Add(s);
                Trim(list);
                continue;
            }
            list.Add(s);
            _latest[key] = s;
            ++AcceptedCount;
            Trim(list);
        }
    }

    private void Trim(List<DeviceSample> list)
    {
        if (list.Count > HistoryLimit) list.RemoveRange(0, list.Count - HistoryLimit);
    }

    /// <summary>按 now 评估质量并取最新值；无样本 → Unknown。</summary>
    public Entry Latest(string deviceId, string signal, DateTime nowUtc)
    {
        var key = (deviceId, signal);
        if (!_latest.TryGetValue(key, out var s))
            return new Entry { Quality = DeviceQuality.Unknown };
        var age = (nowUtc - s.TimestampUtc).TotalSeconds;
        var quality = s.SourceQuality switch
        {
            DeviceQuality.Bad => DeviceQuality.Bad,
            DeviceQuality.Uncertain => DeviceQuality.Uncertain,
            _ => age > StaleWindow.TotalSeconds ? DeviceQuality.Uncertain : DeviceQuality.Good,
        };
        return new Entry { Sample = s, Quality = quality, AgeSeconds = age };
    }

    /// <summary>时间窗内的样本（升序）+ 间隙报告：相邻间隔 > 2×标称周期即缺值段。</summary>
    public (List<DeviceSample> samples, List<(DateTime gapStart, DateTime gapEnd)> gaps) Window(
        string deviceId, string signal, DateTime fromUtc, DateTime toUtc, double nominalPeriodSeconds)
    {
        var key = (deviceId, signal);
        var samples = (_history.TryGetValue(key, out var list) ? list : new List<DeviceSample>())
            .Where(s => s.TimestampUtc >= fromUtc && s.TimestampUtc <= toUtc)
            .OrderBy(s => s.TimestampUtc)
            .ToList();
        var gaps = new List<(DateTime, DateTime)>();
        for (int i = 1; i < samples.Count; ++i)
        {
            var delta = (samples[i].TimestampUtc - samples[i - 1].TimestampUtc).TotalSeconds;
            if (delta > nominalPeriodSeconds * 1.5)
                gaps.Add((samples[i - 1].TimestampUtc, samples[i].TimestampUtc));
        }
        return (samples, gaps);
    }
}

/// <summary>绑定应用（device-data S1）：按质量门限与死区把最新值经宿主 setter
/// 写入目标属性。setter 返回是否接受；单位转换失败 → 报告该项失败不静默。</summary>
public static class DeviceBindingApply
{
    public sealed class Result
    {
        public string Binding = "";
        public DeviceQuality Quality = DeviceQuality.Unknown;
        public bool Applied;
        public double? Value;
        public string? Error;
    }

    public static List<Result> ApplyLatest(
        DeviceDataStore store, IReadOnlyList<DeviceBinding> bindings,
        DateTime nowUtc, Func<DeviceBinding, double, bool> setter)
    {
        var results = new List<Result>();
        foreach (var b in bindings)
        {
            var r = new Result { Binding = $"{b.DeviceId}/{b.Signal} → {b.Component}.{b.Property}" };
            var entry = store.Latest(b.DeviceId, b.Signal, nowUtc);
            r.Quality = entry.Quality;
            if (entry.Quality == DeviceQuality.Unknown)
            {
                r.Error = "无样本";
                results.Add(r);
                continue;
            }
            if (entry.Quality == DeviceQuality.Bad || entry.Quality == DeviceQuality.Uncertain && !b.ApplyWhenUncertain)
            {
                r.Error = $"质量 {entry.Quality} 未达应用门限";
                results.Add(r);
                continue;
            }
            var value = entry.Sample.Value;
            double converted = 0;
            var hasTargetUnit = b.TargetUnit != null;
            if (hasTargetUnit
                && !UnitConversions.TryConvert(entry.Sample.Unit, b.TargetUnit!, value, out converted))
            {
                r.Error = $"单位转换不可用：{entry.Sample.Unit} → {b.TargetUnit}";
                results.Add(r);
                continue;
            }
            if (hasTargetUnit) value = converted;
            r.Value = value;
            r.Applied = setter(b, value);
            results.Add(r);
        }
        return results;
    }
}
