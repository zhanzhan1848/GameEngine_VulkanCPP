using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

// Regression guard for commit 040e06e ("fix(dock): bind Factory +
// InitializeFactory/InitializeLayout to DockControl"). The dock
// control's runtime factory initialization lifecycle runs only
// when both InitializeFactory and InitializeLayout are bound in
// MainWindow.axaml. Removing either causes the layout model to
// exist but render an empty canvas (black middle pane), which the
// existing layout-only tests cannot detect.
public sealed class MainWindowDockControlXamlTests
{
    private const string DockControlElementPattern =
        "<dock:DockControl\\b(?<attrs>(?:[^/>]|/(?!>))*)/>";

    private const string InitializeFactoryAttrPattern =
        "InitializeFactory\\s*=\\s*\"(?<val>[^\"]*)\"";

    private const string InitializeLayoutAttrPattern =
        "InitializeLayout\\s*=\\s*\"(?<val>[^\"]*)\"";

    [Fact]
    public void DockControl_HasInitializeFactoryTrue()
    {
        var element = ExtractDockControlElement();

        var match = Regex.Match(element, InitializeFactoryAttrPattern);
        Assert.True(
            match.Success,
            "MainWindow.axaml's <dock:DockControl> is missing InitializeFactory. " +
            "Re-apply commit 040e06e: the dock control's runtime factory init " +
            "lifecycle will not run without it, leaving a black empty canvas " +
            "even though the layout model in MainWindowViewModel is correct.");
        Assert.Equal("True", match.Groups["val"].Value);
    }

    [Fact]
    public void DockControl_HasInitializeLayoutTrue()
    {
        var element = ExtractDockControlElement();

        var match = Regex.Match(element, InitializeLayoutAttrPattern);
        Assert.True(
            match.Success,
            "MainWindow.axaml's <dock:DockControl> is missing InitializeLayout. " +
            "Re-apply commit 040e06e.");
        Assert.Equal("True", match.Groups["val"].Value);
    }

    [Fact]
    public void DockControl_HasFactoryAndLayoutBindings()
    {
        var element = ExtractDockControlElement();

        Assert.Contains(
            "Factory=\"{Binding Factory}\"",
            element,
            StringComparison.Ordinal);
        Assert.Contains(
            "Layout=\"{Binding Layout}\"",
            element,
            StringComparison.Ordinal);
    }

    private static string ExtractDockControlElement()
    {
        var path = ResolveMainWindowAxamlPath();
        var source = File.ReadAllText(path);

        var match = Regex.Match(source, DockControlElementPattern, RegexOptions.Singleline);
        Assert.True(
            match.Success,
            $"Could not locate <dock:DockControl ... /> element in {path}. " +
            "If the element was renamed, update this test alongside that rename.");

        return match.Value;
    }

    private static string ResolveMainWindowAxamlPath()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 10 && dir is not null; i++, dir = dir.Parent)
        {
            var probe = Path.Combine(dir.FullName, "PrimalEditor_Avalonia", "PrimalEditor_Avalonia.csproj");
            if (File.Exists(probe))
            {
                return Path.Combine(dir.FullName, "PrimalEditor_Avalonia", "Views", "MainWindow.axaml");
            }
        }

        throw new DirectoryNotFoundException(
            "Could not locate PrimalEditor_Avalonia.csproj within 10 parent " +
            "directories of " + AppContext.BaseDirectory);
    }
}
