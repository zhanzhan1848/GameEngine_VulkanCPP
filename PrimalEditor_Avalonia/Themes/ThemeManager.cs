using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml.Styling;
using Avalonia.Styling;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Themes;

/// <summary>
/// Runtime theme switching for the editor front-end. Swaps two things on the
/// UI thread:
///   1. the token ResourceDictionary in <c>Application.Resources.MergedDictionaries</c>
///      (all views reference tokens via {DynamicResource}, so they update live);
///   2. the per-theme global Styles block in <c>Application.Styles</c>
///      (FluentTheme overrides), replacing the StyleInclude that App.axaml
///      booted with, keeping stylesheets at the same position in the list.
/// Selecting a theme persists it via <see cref="EditorThemeSettings"/> and the
/// choice is re-applied by <see cref="Initialize"/> on the next launch.
/// </summary>
public static class ThemeManager
{
    private static EditorTheme _current = EditorThemeRegistry.Find(EditorThemeRegistry.DefaultThemeId)!;
    private static Styles? _attachedStyles;

    /// <summary>The theme whose tokens/styles are currently loaded.</summary>
    public static EditorTheme Current => _current;

    /// <summary>Raised on the UI thread after a theme finished applying.</summary>
    public static event Action<EditorTheme>? ThemeChanged;

    /// <summary>
    /// Apply the persisted theme at startup (no-op when it equals the
    /// App.axaml boot default). Call once from the UI thread.
    /// </summary>
    public static void Initialize()
    {
        var saved = EditorThemeSettings.Load().ThemeId;
        if (string.IsNullOrEmpty(saved) || saved == _current.Id) return;
        if (EditorThemeRegistry.Find(saved) == null)
        {
            Logger.Warning($"ThemeManager: unknown saved theme '{saved}'; keeping {_current.Id}");
            return;
        }
        try
        {
            Apply(saved);
        }
        catch (Exception ex)
        {
            Logger.Error($"ThemeManager: failed to apply saved theme '{saved}'; keeping {_current.Id}", ex);
        }
    }

    /// <summary>Switch to a theme by registry id. Returns false for unknown ids.</summary>
    public static bool Apply(string themeId)
    {
        var theme = EditorThemeRegistry.Find(themeId);
        if (theme == null) return false;
        if (theme.Id == _current.Id) return true;
        Apply(theme);
        return true;
    }

    /// <summary>Switch to a theme immediately (UI thread).</summary>
    public static void Apply(EditorTheme theme)
    {
        if (!Dispatcher.UIThread.CheckAccess())
            throw new InvalidOperationException("ThemeManager.Apply must run on the UI thread");

        var app = Application.Current ?? throw new InvalidOperationException("Application not started");

        // 1. Tokens first: DynamicResource consumers re-resolve immediately.
        //    (The Uri ctor arg is the XAML context baseUri, NOT Source —
        //    Source must be assigned explicitly or loading throws.)
        var merged = app.Resources.MergedDictionaries;
        RemoveRegisteredThemeResources(merged);
        merged.Add(new ResourceInclude(theme.ResourcesUri) { Source = theme.ResourcesUri });

        // 2. Styles: remove boot StyleInclude (App.axaml) + our previous
        //    block, then add the new one so it keeps overriding the base
        //    FluentTheme. Each apply instantiates a FRESH Styles —
        //    re-attaching a StyleInclude whose Source was loaded before
        //    (or reusing the removed instance) leaves stale attach state
        //    and the setters silently stop applying.
        var appStyles = app.Styles;
        RemoveRegisteredThemeStyles(appStyles);
        var styles = (Styles)Avalonia.Markup.Xaml.AvaloniaXamlLoader.Load(theme.StylesUri);
        appStyles.Add(styles);
        _attachedStyles = styles;

        // 3. Fluent variant: light skins need Fluent's popup/menu/checkbox
        //    templates re-resolved as Light (App.axaml boots pinned Dark).
        if (theme.Variant != ThemeVariant.Default)
            app.RequestedThemeVariant = theme.Variant;

        _current = theme;
        EditorThemeSettings.Save(new EditorThemeConfig(theme.Id));
        Logger.Info($"ThemeManager: applied theme '{theme.Id}'");
        ThemeChanged?.Invoke(theme);
    }

    private static void RemoveRegisteredThemeResources(IList<IResourceProvider> merged)
    {
        for (var i = merged.Count - 1; i >= 0; i--)
        {
            if (merged[i] is ResourceInclude { Source: not null } include &&
                EditorThemeRegistry.All.Any(t => t.ResourcesUri == include.Source))
            {
                merged.RemoveAt(i);
            }
        }
    }

    private static void RemoveRegisteredThemeStyles(Styles appStyles)
    {
        for (var i = appStyles.Count - 1; i >= 0; i--)
        {
            if (appStyles[i] is StyleInclude { Source: not null } include &&
                EditorThemeRegistry.All.Any(t => t.StylesUri == include.Source))
            {
                appStyles.RemoveAt(i);
            }
        }
        if (_attachedStyles != null)
        {
            appStyles.Remove(_attachedStyles);
            _attachedStyles = null;
        }
    }
}
