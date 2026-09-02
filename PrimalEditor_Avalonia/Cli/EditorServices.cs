using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Composition root for CLI command handlers: holds the live editor objects
/// (main window / world editor VM) and resolves viewports. Populated by
/// <see cref="App"/> when the main window is created; every accessor is
/// null-tolerant because commands can arrive before/after the UI exists.
///
/// All command handlers run on the UI thread (the CliHost marshaler
/// guarantees it), so these properties are read without locking.
/// </summary>
public static class EditorServices
{
    public static MainWindow? MainWindow { get; private set; }
    public static MainWindowViewModel? MainWindowViewModel { get; private set; }

    /// <summary>测试注入点：绕过 Avalonia MainWindow 直接指定世界编辑器 VM
    ///（PrimalEditor_Avalonia.Tests 经 InternalsVisibleTo 使用）。生产路径恒为 null。</summary>
    internal static WorldEditorViewModel? WorldEditorOverride;

    private static DateTime _startedAtUtc = DateTime.UtcNow;

    public static void Attach(MainWindow mainWindow, MainWindowViewModel viewModel)
    {
        MainWindow = mainWindow;
        MainWindowViewModel = viewModel;
        _startedAtUtc = DateTime.UtcNow;
    }

    public static WorldEditorViewModel? WorldEditor => WorldEditorOverride ?? MainWindowViewModel?.WorldEditor;

    public static Scene? ActiveScene => WorldEditor?.ActiveScene;

    public static TimeSpan Uptime => DateTime.UtcNow - _startedAtUtc;

    /// <summary>
    /// The single EngineViewport3D in the main window's visual tree, or null
    /// before the world editor view is realized (project not open yet).
    /// </summary>
    public static EngineViewport3D? FindViewport()
    {
        if (MainWindow == null) return null;
        return MainWindow.GetVisualDescendants().OfType<EngineViewport3D>().FirstOrDefault();
    }
}
