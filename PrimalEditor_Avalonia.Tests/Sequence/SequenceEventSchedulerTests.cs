using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Sequence;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.SequenceNs;

/// <summary>
/// sequence S2 事件轨契约测试：恰一次触发（逐帧推进跨触发帧）、seek 跳帧
/// 零触发、事件有序（帧序+名字 Ordinal）、回绕不补发、Reset 后每圈正常、
/// 序列化往返保留事件轨。
/// </summary>
public sealed class SequenceEventSchedulerTests
{
    private static SequenceDocument Doc() => new()
    {
        DurationFrames = 120,
        Tracks =
        {
            new EventTrack
            {
                Name = "main",
                Keys =
                {
                    new EventKey { Frame = 10, Event = "door-open" },
                    new EventKey { Frame = 30, Event = "alpha" },
                    new EventKey { Frame = 30, Event = "beta" },    // 同帧双事件（序确定）
                    new EventKey { Frame = 60, Event = "finale" },
                },
            },
        },
    };

    [Fact]
    public void FrameByFrameAdvance_FiresExactlyOnce()
    {
        var fired = new List<string>();
        var scheduler = new SequenceEventScheduler(Doc(), e => fired.Add(e.Event));

        scheduler.Advance(0);                          // 起点
        for (var f = 1; f <= 120; f++) scheduler.Advance(f);

        Assert.Equal(new[] { "door-open", "alpha", "beta", "finale" }, fired);   // 有序
        Assert.Equal(4, fired.Count);                  // 恰一次（无重复）
    }

    [Fact]
    public void MultiFrameAdvance_CoversRangeOnce()
    {
        var fired = new List<string>();
        var scheduler = new SequenceEventScheduler(Doc(), e => fired.Add(e.Event));
        scheduler.Advance(0);
        var step1 = scheduler.Advance(35);             // 一次跨 1..35
        Assert.Equal(3, step1.Count);                  // door-open + alpha + beta
        var step2 = scheduler.Advance(120);            // 再跨 36..120
        Assert.Single(step2, e => e.Event == "finale");
        // 再推进（尾后）无新事件
        Assert.Empty(scheduler.Advance(120));
        Assert.Equal(4, fired.Count);
    }

    [Fact]
    public void Seek_DoesNotFire_BusinessEvents()
    {
        var fired = new List<string>();
        var scheduler = new SequenceEventScheduler(Doc(), e => fired.Add(e.Event));
        scheduler.Advance(0);

        scheduler.Seek(100);                           // 跳到 100（跨过 10/30/60）
        Assert.Empty(fired);                           // 零误触
        Assert.Equal(100, scheduler.LastFrame);

        // seek 后继续前进：只触发 (100, …] 的未来事件
        scheduler.Advance(110);
        Assert.Empty(fired);                           // 60 已被"跳过"

        // 回到起点前 Reset 再播：正常触发
        scheduler.Reset(0);
        scheduler.Advance(0);
        scheduler.Advance(12);
        Assert.Equal(new[] { "door-open" }, fired);
    }

    [Fact]
    public void Loop_WithoutReset_DoesNotRefire_Retroactively()
    {
        var fired = new List<string>();
        var scheduler = new SequenceEventScheduler(Doc(), e => fired.Add(e.Event));
        scheduler.Advance(0);
        scheduler.Advance(65);                          // door-open/alpha/beta/finale 已发
        Assert.Equal(4, fired.Count);

        scheduler.Advance(0);                           // 回绕（未 Reset）：不补发
        Assert.Equal(4, fired.Count);

        scheduler.Reset(0);                             // 显式新一圈
        scheduler.Advance(0);
        scheduler.Advance(65);
        Assert.Equal(8, fired.Count);                   // 第二圈完整重发
    }

    [Fact]
    public void EventsBetween_OrderingAndBounds()
    {
        var doc = Doc();
        // (0, 30]：door-open(10) + alpha/beta(30, 同帧名字 Ordinal)
        var events = SequenceEvaluator.EventsBetween(doc, 0, 30);
        Assert.Equal(new[] { "door-open", "alpha", "beta" },
            events.Select(e => e.Event));

        // 左开右含：from=30 不含 30 的事件
        Assert.Empty(SequenceEvaluator.EventsBetween(doc, 30, 30));
        Assert.Single(SequenceEvaluator.EventsBetween(doc, 30, 60), e => e.Event == "finale");

        // 不后退
        Assert.Empty(SequenceEvaluator.EventsBetween(doc, 60, 30));
    }

    [Fact]
    public void Serialization_RoundTrip_PreservesEventTrack()
    {
        var doc = Doc();
        var path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"evt_{Guid.NewGuid():N}.pseq");
        doc.Save(path);
        var loaded = SequenceDocument.Load(path);
        System.IO.File.Delete(path);

        var track = loaded.FindEventTrack();
        Assert.NotNull(track);
        Assert.Equal(4, track!.Keys.Count);
        Assert.Equal("finale", track.Keys.Single(k => k.Frame == 60).Event);

        // 重载后调度行为一致
        var fired = new List<string>();
        var scheduler = new SequenceEventScheduler(loaded, e => fired.Add(e.Event));
        scheduler.Advance(0);
        scheduler.Advance(60);
        Assert.Equal(new[] { "door-open", "alpha", "beta", "finale" }.Take(4), fired.Take(4));
    }
}
