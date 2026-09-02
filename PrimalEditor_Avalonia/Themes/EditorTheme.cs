using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Themes;

/// <summary>
/// Immutable descriptor of one editor UI skin (前端外观,不触碰引擎与 ViewModel)。
/// 每套主题由两个 avares 资源构成:
///   - Resources: 令牌 ResourceDictionary(颜色/画刷/字体/阴影),所有主题共用同一套 key;
///   - Styles:    全局控件 Styles,通过选择器覆写 FluentTheme,值引用主题令牌。
/// </summary>
public sealed record EditorTheme(
    string Id,
    string DisplayName,
    string ResourcesPath,
    string StylesPath)
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
    public const string DefaultThemeId = CyberpunkId;
    public const string CyberpunkId = "cyberpunk";
    public const string WarcraftId = "warcraft";

    public static IReadOnlyList<EditorTheme> All { get; } = new[]
    {
        new EditorTheme(
            CyberpunkId,
            "Cyberpunk",
            "Themes/Cyberpunk/CyberpunkTheme.axaml",
            "Themes/Cyberpunk/CyberpunkTheme.Styles.axaml"),
        new EditorTheme(
            WarcraftId,
            "Warcraft",
            "Themes/Warcraft/WarcraftTheme.axaml",
            "Themes/Warcraft/WarcraftTheme.Styles.axaml"),
    };

    public static EditorTheme? Find(string? id) => All.FirstOrDefault(t => t.Id == id);
}
