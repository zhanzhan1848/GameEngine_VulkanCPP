using System.Collections.Generic;
using PrimalEditor_Avalonia.Ui;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.UiNs;

/// <summary>
/// runtime-ui S1 后端契约测试：控件树+绑定求值（命中/缺失不静默/绑定优先/
/// 嵌套路径/类型转换）、序列化往返+未知版本拒、校验（空/重复 ID/未知类型）。
/// </summary>
public sealed class UiDocumentTests
{
    private static UiDocument Hud() => new()
    {
        Root = new UiNode
        {
            Id = "root", Type = "StackPanel",
            Children =
            {
                new UiNode
                {
                    Id = "health", Type = "Text",
                    Properties = { ["text"] = "HP --" },
                    Bindings = { new UiBinding { Path = "player.health", Property = "text" } },
                },
                new UiNode
                {
                    Id = "btn", Type = "Button",
                    Properties = { ["text"] = "Attack" },
                },
            },
        },
    };

    private static IReadOnlyDictionary<string, object> Context() =>
        new Dictionary<string, object>
        {
            ["player"] = new Dictionary<string, object>
            {
                ["health"] = 87.5,
                ["name"] = "Hero",
                ["alive"] = true,
            },
        };

    [Fact]
    public void Evaluate_BindingWins_MissingIsFlagged()
    {
        var resolved = UiEvaluator.Evaluate(Hud(), Context());
        var health = resolved.Children[0];
        Assert.Equal("87.5", health.Values["text"]);            // 绑定值覆盖静态
        Assert.Equal("ok", health.BindingStatus["player.health"]);
        var btn = resolved.Children[1];
        Assert.Equal("Attack", btn.Values["text"]);             // 无绑定用静态
        Assert.Empty(btn.BindingStatus);
    }

    [Fact]
    public void Evaluate_MissingPath_KeepsStaticAndFlags()
    {
        var resolved = UiEvaluator.Evaluate(Hud(), null);       // 无上下文
        var health = resolved.Children[0];
        Assert.Equal("HP --", health.Values["text"]);           // 保持静态占位
        Assert.Equal("missing", health.BindingStatus["player.health"]);   // 不静默
    }

    [Fact]
    public void Evaluate_TypeConversions_CultureInvariant()
    {
        var resolved = UiEvaluator.Evaluate(
            new UiDocument
            {
                Root = new UiNode
                {
                    Id = "r", Type = "Text",
                    Bindings =
                    {
                        new UiBinding { Path = "v.d", Property = "d" },
                        new UiBinding { Path = "v.b", Property = "b" },
                        new UiBinding { Path = "v.i", Property = "i" },
                    },
                },
            },
            new Dictionary<string, object>
            {
                ["v"] = new Dictionary<string, object> { ["d"] = 3.5, ["b"] = false, ["i"] = 42 },
            });
        Assert.Equal("3.5", resolved.Values["d"]);
        Assert.Equal("false", resolved.Values["b"]);
        Assert.Equal("42", resolved.Values["i"]);
    }

    [Fact]
    public void Document_RoundTrip_AndValidation()
    {
        var doc = Hud();
        Assert.Empty(doc.Validate());
        var (loaded, err) = UiDocument.Load(doc.Serialize());
        Assert.Null(err);
        Assert.Equal("health", loaded!.Root.Children[0].Id);

        var (future, futureErr) = UiDocument.Load(
            doc.Serialize().Replace("\"schemaVersion\": 1", "\"schemaVersion\": 7"));
        Assert.Null(future);
        Assert.Contains("不静默覆写", futureErr);

        var bad = Hud();
        bad.Root.Children[1].Id = "health";                 // 重复 ID
        bad.Root.Children[1].Type = "Hologram";             // 未知类型
        var errors = bad.Validate();
        Assert.Contains(errors, e => e.Contains("重复节点 Id: health"));
        Assert.Contains(errors, e => e.Contains("Hologram"));
    }
}

