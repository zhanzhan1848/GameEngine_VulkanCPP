using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia.Styling;

namespace PrimalEditor_Avalonia.Themes;

/// <summary>
/// Immutable descriptor of one editor UI skin (前端外观,不触碰引擎与 ViewModel)。
/// 每套主题由两个 avares 资源构成:
///   - Resources: 令牌 ResourceDictionary(颜色/画刷/字体/阴影),所有主题共用同一套 key;
///   - Styles:    全局控件 Styles,通过选择器覆写 FluentTheme,值引用主题令牌。
/// Variant 是该主题要求的 FluentTheme 变体:浅色皮肤必须是 Light,否则 Fluent
/// 拥有的弹层/下拉/复选框等模板会按 Application 级变体(App.axaml 钉死 Dark)
/// 渲染成深色。Default 表示不改启动时的变体。
/// </summary>
public sealed record EditorTheme(
    string Id,
    string DisplayName,
    string ResourcesPath,
    string StylesPath,
    ThemeVariant Variant = default)
{
    public Uri ResourcesUri => new($"avares://PrimalEditor_Avalonia/{ResourcesPath}");
    public Uri StylesUri => new($"avares://PrimalEditor_Avalonia/{StylesPath}");

    public override string ToString() => DisplayName;
}

/// <summary>
/// All registered themes. Adding a skin = create the two axaml files under
/// Themes/&lt;Name&gt;/ and append one entry here; the menu and persistence
/// pick it up automatically.
/// </summary>
public static class EditorThemeRegistry
{
    public const string DefaultThemeId = WarcraftId;
    public const string CyberpunkId = "cyberpunk";
    public const string WarcraftId = "warcraft";
    public const string LiquidGlassId = "liquid-glass";

    public static IReadOnlyList<EditorTheme> All { get; } = new[]
    {
        new EditorTheme(
            WarcraftId,
            "Warcraft",
            "Themes/Warcraft/WarcraftTheme.axaml",
            "Themes/Warcraft/WarcraftTheme.Styles.axaml",
            ThemeVariant.Dark),
        new EditorTheme(
            CyberpunkId,
            "Cyberpunk",
            "Themes/Cyberpunk/CyberpunkTheme.axaml",
            "Themes/Cyberpunk/CyberpunkTheme.Styles.axaml",
            ThemeVariant.Dark),
        new EditorTheme(
            LiquidGlassId,
            "Liquid Glass",
            "Themes/LiquidGlass/LiquidGlassTheme.axaml",
            "Themes/LiquidGlass/LiquidGlassTheme.Styles.axaml",
            ThemeVariant.Light),
    };

    public static EditorTheme? Find(string? id) => All.FirstOrDefault(t => t.Id == id);
}
