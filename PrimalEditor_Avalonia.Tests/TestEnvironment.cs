using System;
using System.IO;
using System.Runtime.CompilerServices;

namespace PrimalEditor_Avalonia.Tests;

/// <summary>
/// Redirects ThemeManager persistence to a temp file so headless theme
/// tests never touch the user's real editor-theme.json. Runs at assembly
/// load, before any test can trigger a theme apply.
/// </summary>
internal static class TestEnvironment
{
    [ModuleInitializer]
    internal static void Init()
    {
        var path = Path.Combine(Path.GetTempPath(), "primal-editor-theme-tests", "editor-theme.json");
        Environment.SetEnvironmentVariable("PRIMAL_EDITOR_THEME_FILE", path);
    }
}
