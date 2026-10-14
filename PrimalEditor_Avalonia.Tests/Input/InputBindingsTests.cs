using System;
using System.Linq;
using PrimalEditor_Avalonia.Input;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.InputNs;

/// <summary>
/// input-camera S2 数据层契约测试：解析（头/注释/坏行/无键丢弃）、确定性序列化
/// 往返、重绑（移动语义/幂等）、冲突检测（既有+预检）、死区越界拒绝、恢复默认。
/// </summary>
public sealed class InputBindingsTests
{
    [Fact]
    public void Parse_And_RoundTrip_Deterministic()
    {
        var text = "# primal-input-bindings v1\n"
                 + "action MoveForward W UpArrow\n"
                 + "\n"
                 + "# comment\n"
                 + "action Jump Space\n";
        var (b, errors) = InputBindings.Parse(text);
        Assert.NotNull(b);
        Assert.Empty(errors);
        Assert.Equal(new[] { "Jump", "MoveForward" }, b!.Actions);   // Ordinal 排序
        Assert.Equal(new[] { "UpArrow", "W" }, b.KeysOf("MoveForward"));

        var (again, _) = InputBindings.Parse(b.Serialize());
        Assert.Equal(b.Actions, again!.Actions);
        Assert.Equal(b.KeysOf("MoveForward"), again.KeysOf("MoveForward"));
        Assert.Equal(b.KeysOf("Jump"), again.KeysOf("Jump"));
    }

    [Fact]
    public void Parse_BadLines_AreErrors_NotSilent()
    {
        var text = InputBindings.FileHeader + "\n"
                 + "garbage line here\n"          // 非 action 行
                 + "action NoKeys\n"               // 无键
                 + "action OK K\n";
        var (b, errors) = InputBindings.Parse(text);
        Assert.NotNull(b);
        Assert.Equal(2, errors.Count);
        Assert.Contains(errors, e => e.Contains("第 2 行"));
        Assert.Contains(errors, e => e.Contains("NoKeys"));
        Assert.Equal(new[] { "OK" }, b!.Actions);
    }

    [Fact]
    public void Parse_EmptyText_Rejected()
    {
        var (b, errors) = InputBindings.Parse("   \n");
        Assert.Null(b);
        Assert.Contains(errors, e => e.Contains("为空"));
    }

    [Fact]
    public void Rebind_MovesKey_Idempotent()
    {
        var b = InputBindings.DefaultPlayer();
        b.Rebind(to: "Sprint", key: "W", from: "MoveForward");
        Assert.Equal(new[] { "UpArrow" }, b.KeysOf("MoveForward"));
        Assert.Equal(new[] { "W" }, b.KeysOf("Sprint"));

        b.Rebind(to: "Sprint", key: "W");          // 再绑同键=幂等
        Assert.Equal(new[] { "W" }, b.KeysOf("Sprint"));

        b.Unbind("Sprint", "W");
        Assert.Empty(b.KeysOf("Sprint"));           // 空集动作移除
        Assert.Empty(b.Actions.Where(a => a == "Sprint"));
    }

    [Fact]
    public void Conflicts_Detected_ExistingAndPreview()
    {
        var b = InputBindings.DefaultPlayer();
        Assert.Empty(b.Conflicts());                // 默认无冲突

        b.Bind("Crouch", "W");                      // W 同时绑 MoveForward/Crouch
        var conflict = Assert.Single(b.Conflicts());
        Assert.Equal("W", conflict.Key);
        Assert.Equal(new[] { "Crouch", "MoveForward" }, conflict.Actions);

        var c2 = InputBindings.DefaultPlayer();
        var preview = c2.ConflictsFor(to: "Crouch", key: "W");   // 预检不改动
        Assert.Single(preview);
        Assert.Contains("Crouch", preview[0].Actions);
        Assert.Contains("MoveForward", preview[0].Actions);
        Assert.Empty(c2.Conflicts());               // 预检零副作用
        Assert.Empty(c2.KeysOf("Crouch"));
    }

    [Fact]
    public void Deadzone_Validation()
    {
        var b = InputBindings.DefaultPlayer();
        Assert.True(b.SetDeadzone("AxisLeftX", 0.15));
        Assert.False(b.SetDeadzone("AxisLeftX", -0.1));
        Assert.False(b.SetDeadzone("AxisLeftX", 1.0));
        Assert.Equal(0.15, b.Deadzones["AxisLeftX"]);
    }

    [Fact]
    public void ResetToDefault_Restores()
    {
        var b = InputBindings.DefaultPlayer();
        b.Rebind(to: "Sprint", key: "W", from: "MoveForward");
        b.Bind("Extra", "P");
        b.SetDeadzone("AxisLeftX", 0.2);

        b.ResetToDefault();
        Assert.Empty(b.Conflicts());
        Assert.Empty(b.Deadzones);
        Assert.Equal(new[] { "UpArrow", "W" }, b.KeysOf("MoveForward"));
        Assert.Empty(b.Actions.Where(a => a == "Sprint" || a == "Extra"));
    }
}
