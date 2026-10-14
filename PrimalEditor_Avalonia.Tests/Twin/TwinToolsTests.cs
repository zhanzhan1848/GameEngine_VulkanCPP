using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.DeviceData;
using PrimalEditor_Avalonia.Twin;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.TwinNs;

/// <summary>
/// twin-tools S1 契约测试：层级查询（按名/路径/子树/重复 ID 拒绝/孤儿报告）、
/// 告警状态机（触发/滞回解除/持续时长/未解除区间）、历史回放（as-of 二分、
/// 间隙 Unknown、区间边界）。
/// </summary>
public sealed class TwinToolsTests
{
    private static readonly DateTime Base = new(2026, 10, 15, 8, 0, 0, DateTimeKind.Utc);

    [Fact]
    public void Registry_PathAndSubtree_Queries()
    {
        var reg = new TwinRegistry();
        reg.Register(new TwinNode { Id = "s1", Kind = "site", Name = "plantA" });
        reg.Register(new TwinNode { Id = "d1", Kind = "device", Name = "furnace", ParentId = "s1", DeviceId = "sim-furnace-01" });
        reg.Register(new TwinNode { Id = "p1", Kind = "part", Name = "bearing", ParentId = "d1" });
        reg.Register(new TwinNode { Id = "p2", Kind = "part", Name = "bearing", ParentId = "d1" });   // 同名不同 id

        Assert.Equal("d1", reg.FindByPath("plantA/furnace")!.Id);
        Assert.Equal("p1", reg.FindByPath("plantA/furnace/bearing")!.Id);   // 同名取先注册
        Assert.Null(reg.FindByPath("plantA/bearing"));                       // 层级不符
        Assert.Null(reg.FindByPath("plantA/missing"));

        var subtree = reg.Subtree(reg.FindByName("furnace")!);
        Assert.Equal(3, subtree.Count);                                      // furnace + 2 bearing
        Assert.Contains(subtree, n => n.Id == "s1" ? false : n.Id == "p2");
    }

    [Fact]
    public void Registry_DuplicateIdRejected_OrphansReported()
    {
        var reg = new TwinRegistry();
        reg.Register(new TwinNode { Id = "a", Name = "A" });
        Assert.Throws<InvalidOperationException>(
            () => reg.Register(new TwinNode { Id = "a", Name = "again" }));
        reg.Register(new TwinNode { Id = "b", Name = "B", ParentId = "ghost" });
        var orphan = Assert.Single(reg.Orphans());
        Assert.Equal("b", orphan.Id);   // 容忍注册但诊断可见——不静默
    }

    private static DeviceDataStore MakeStore(params (DateTime t, double v)[] samples)
    {
        var store = new DeviceDataStore();
        store.Push(samples.Select(s => new DeviceSample
        {
            TimestampUtc = s.t, DeviceId = "d", Signal = "temp", Value = s.v, Unit = "degC",
        }));
        return store;
    }

    [Fact]
    public void Alarm_FiresAndClearsWithHysteresis()
    {
        var store = MakeStore(
            (Base, 20), (Base.AddSeconds(1), 21), (Base.AddSeconds(2), 25),   // 越过 24
            (Base.AddSeconds(3), 24.5),                                        // 带内不清除
            (Base.AddSeconds(4), 22.5),                                        // 回落出滞回带（≤23）清除
            (Base.AddSeconds(5), 30));                                         // 再次触发
        var rule = new AlarmRule
        {
            RuleId = "high-temp", DeviceId = "d", Signal = "temp",
            Comparator = ">", Threshold = 24, Hysteresis = 1,
        };
        var events = AlarmEvaluator.Evaluate(store, new[] { rule }, "d", "temp",
            Base, Base.AddSeconds(6), 1.0);
        Assert.Equal(2, events.Count);
        Assert.Equal(Base.AddSeconds(2), events[0].FiredAt);                  // 起点可追溯
        Assert.Equal(Base.AddSeconds(4), events[0].ClearedAt);                // 带内不清除
        Assert.Null(events[1].ClearedAt);                                     // 持续到窗尾
    }

    [Fact]
    public void Alarm_SustainRequiresContinuousHold()
    {
        // 25 出现一瞬后回落——SustainSeconds=2 下不触发
        var store = MakeStore(
            (Base, 20), (Base.AddSeconds(1), 25), (Base.AddSeconds(2), 21),
            (Base.AddSeconds(3), 25), (Base.AddSeconds(4), 26), (Base.AddSeconds(5), 25));
        var rule = new AlarmRule
        {
            RuleId = "sustained", DeviceId = "d", Signal = "temp",
            Comparator = ">", Threshold = 24, SustainSeconds = 2,
        };
        var events = AlarmEvaluator.Evaluate(store, new[] { rule }, "d", "temp",
            Base, Base.AddSeconds(6), 1.0);
        var ev = Assert.Single(events);
        Assert.Equal(Base.AddSeconds(3), ev.FiredAt);   // 持续条件起点
    }

    [Fact]
    public void Playback_AsOfValue_BinarySearchAndGaps()
    {
        var store = MakeStore(
            (Base, 20), (Base.AddSeconds(1), 21), (Base.AddSeconds(2), 22),
            (Base.AddSeconds(10), 30));   // 2..10 为间隙
        var playback = new HistoryPlayback(store, "d", new[] { "temp" },
            Base, Base.AddSeconds(11), 1.0);
        Assert.Equal(20, playback.ValueAt("temp", Base.AddMilliseconds(500))!.Value.value, 6);
        Assert.Equal(22, playback.ValueAt("temp", Base.AddSeconds(9.9))!.Value.value, 6);   // as-of 持续
        Assert.Equal(30, playback.ValueAt("temp", Base.AddSeconds(10))!.Value.value, 6);
        Assert.Null(playback.ValueAt("temp", Base.AddSeconds(-1)));                    // 窗前
        Assert.Null(playback.ValueAt("missing", Base));                                // 无此信号
    }
}
