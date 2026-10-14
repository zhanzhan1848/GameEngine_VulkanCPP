using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Scripting;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ScriptingNs;

/// <summary>
/// scripting S2 属性/事件契约测试（A01）：
/// 订阅一次触发一次；重载 10 次仍一次；setter 范围拒绝且不改值；Undo 恢复旧值；
/// 类型/只读/所有权冲突全部拒绝；批量 set 部分成功清单。
/// </summary>
public sealed class PropertyContractTests
{
    private static PropertySet MakeProps()
    {
        var set = new PropertySet();
        set.Define(new PropertyMetadata
        {
            StableId = "player.speed", Type = PropertyTypeCode.Number,
            MinValue = 0, MaxValue = 10, Unit = "m/s",
        }, 5.0);
        set.Define(new PropertyMetadata
        {
            StableId = "engine.fps", Type = PropertyTypeCode.Number, ReadOnly = true, Owner = "engine",
        }, 60.0);
        set.Define(new PropertyMetadata
        {
            StableId = "player.name", Type = PropertyTypeCode.String,
        }, "Hero");
        return set;
    }

    [Fact]
    public void Set_RangeRejects_AndDoesNotChangeValue()
    {
        var set = MakeProps();
        var speed = set.Find("player.speed")!;
        Assert.False(speed.TrySet(-1.0, "user", out var e1));
        Assert.Contains("下限", e1);
        Assert.False(speed.TrySet(11.0, "user", out var e2));
        Assert.Contains("上限", e2);
        Assert.Equal(5.0, speed.Value);   // 拒绝不改值
        Assert.True(speed.TrySet(7.5, "user", out _));
        Assert.Equal(7.5, speed.Value);
    }

    [Fact]
    public void Set_TypeReadOnlyOwnership_AllRejected()
    {
        var set = MakeProps();
        Assert.False(set.Find("player.speed")!.TrySet("fast", "user", out var e1));
        Assert.Contains("需要 Number", e1);
        Assert.False(set.Find("engine.fps")!.TrySet(120.0, "engine", out var e2));
        Assert.Contains("只读", e2);
        Assert.False(set.Find("engine.fps")!.TrySet(120.0, "user", out var e3));
        Assert.Contains("只读", e3);      // 只读先拒——所有权用非只读项验证：
        // 构造 engine-owned 可写属性
        var owned = new ObservableProperty(new PropertyMetadata
        {
            StableId = "sim.state", Type = PropertyTypeCode.Number, Owner = "engine",
        }, 1.0);
        Assert.False(owned.TrySet(2.0, "script", out var e4));
        Assert.Contains("归 engine 所有", e4);
        Assert.True(owned.TrySet(2.0, "engine", out _));
    }

    [Fact]
    public void Undo_RestoresOldValue()
    {
        var set = MakeProps();
        var (applied, rejected) = set.SetMany(new[]
        {
            ("player.speed", (object)8.0),
            ("player.name", (object)"Rogue"),
        }, "user");
        Assert.Empty(rejected);
        Assert.Equal(2, applied.Count);
        foreach (var a in applied.AsEnumerable().Reverse()) a.Undo();
        Assert.Equal(5.0, set.Find("player.speed")!.Value);
        Assert.Equal("Hero", set.Find("player.name")!.Value);
        foreach (var a in applied) a.Redo();
        Assert.Equal(8.0, set.Find("player.speed")!.Value);
        Assert.Equal("Rogue", set.Find("player.name")!.Value);
    }

    [Fact]
    public void SetMany_PartialSuccessReportsBoth()
    {
        var set = MakeProps();
        var (applied, rejected) = set.SetMany(new[]
        {
            ("player.speed", (object)3.0),      // ok
            ("player.speed", (object)99.0),     // 上限拒
            ("missing.prop", (object)1.0),      // 未注册拒
        }, "user");
        Assert.Single(applied);
        Assert.Equal(2, rejected.Count);
        Assert.Equal(3.0, set.Find("player.speed")!.Value);
    }

    [Fact]
    public void EventBus_FiresOncePerPublish_AndReloadSafety()
    {
        var bus = new ScriptEventBus();
        var count = 0;
        Action<object?> handler = _ => ++count;
        var token = bus.Subscribe("hit", handler);
        Assert.Equal(1, bus.Publish("hit", null));
        Assert.Equal(1, count);   // 一次投递

        // 同一委托重复订阅不叠加（防重载重复订阅）
        bus.Subscribe("hit", handler);
        bus.Publish("hit", null);
        Assert.Equal(2, count);

        // 重载 10 次：每次清空重订（模拟脚本 reload 生命周期）
        for (int i = 0; i < 10; ++i)
        {
            bus.OnScriptReload();
            Assert.False(token.Active);          // 旧 token 失效
            bus.Unsubscribe(token);              // 幂等
            var fresh = bus.Subscribe("hit", _ => ++count);
            bus.Publish("hit", null);
            Assert.Equal(3 + i, count);          // 每轮恰 +1
            bus.Unsubscribe(fresh);
            bus.Publish("hit", null);            // 已解绑：不投递
            Assert.Equal(3 + i, count);
        }
    }

    [Fact]
    public void EventBus_PublishDuringHandler_SeesStableSnapshot()
    {
        var bus = new ScriptEventBus();
        var order = new List<string>();
        bus.Subscribe("e", _ =>
        {
            order.Add("first");
            bus.Subscribe("e", _ => order.Add("late"));   // 发布中订阅：不影响本轮
        });
        bus.Subscribe("e", _ => order.Add("second"));
        bus.Publish("e", null);
        Assert.Equal(new[] { "first", "second" }, order);
        bus.Publish("e", null);
        Assert.Equal(new[] { "first", "second", "first", "second", "late" }, order);
    }
}
