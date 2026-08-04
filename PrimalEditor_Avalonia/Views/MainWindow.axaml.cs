using System.IO;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class MainWindow : Window
{
    private bool _reopenAttempted;

    public MainWindow()
    {
        InitializeComponent();
        Opened += OnMainWindowOpened;
    }

    /// <summary>
    /// Restore the asset windows that were open at the end of the
    /// previous editor session, as recorded by
    /// <see cref="EditorLayoutSettings"/>. Runs once per process to
    /// avoid re-opening on every layout pass.
    /// </summary>
    private void OnMainWindowOpened(object? sender, System.EventArgs e)
    {
        if (_reopenAttempted) return;
        _reopenAttempted = true;
        try
        {
            ReopenLastSessionAssets();
        }
        catch (System.Exception ex)
        {
            Logger.Error("MainWindow: failed to reopen last session", ex);
        }
    }

    private void ReopenLastSessionAssets()
    {
        var layout = EditorLayoutSettings.Load();
        // Expose the layout to AssetEditorRegistry so each Open can
        // restore its persisted window state (bounds, maximized).
        AssetEditorRegistry.SetActiveLayout(layout);
        foreach (var path in layout.OpenAssets)
        {
            if (!File.Exists(path))
            {
                Logger.Info($"MainWindow: skip missing asset on reopen: {path}");
                continue;
            }
            var info = AssetRegistry.Get(path) ?? new AssetInfo
            {
                FullPath = path,
                Type = ContentHelper.GetAssetType(path),
            };
            AssetEditorRegistry.Open(info);
        }
        Logger.Info($"MainWindow: reopen-session finished ({layout.OpenAssets.Count} candidate path(s))");
    }

    private void OnToggleLogger(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.ToggleLogger();
        }
    }

    private async void OnImportAssetClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm && vm.Project != null)
        {
            await vm.ContentBrowser.ImportAssetAsync();
        }
        else
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("Open a project first to import assets");
        }
    }

    private async void OnNewScriptClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ViewModels.MainWindowViewModel vm) return;
        if (vm.Project == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("New Script: no project loaded");
            return;
        }
        var dlg = new Views.NewScriptDialog();
        var top = TopLevel.GetTopLevel(this) as Window;
        if (top == null) return;
        var result = await dlg.ShowDialogAsync(top);
        if (result == null) return;
        var scriptsDir = System.IO.Path.Combine(vm.Project.ContentPath, "Scripts");
        System.IO.Directory.CreateDirectory(scriptsDir);
        var ext = result.Language == "lua" ? ".lua" : ".py";
        var path = System.IO.Path.Combine(scriptsDir, result.Name + ext);
        try
        {
            System.IO.File.WriteAllText(path, ScriptStub(result.Language, result.Name));
            PrimalEditor_Avalonia.Content.AssetRegistry.Register(path);
            PrimalEditor_Avalonia.Common.Logger.Info($"New script created: {path}");
        }
        catch (System.Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("New Script failed", ex);
        }
    }

    private static string ScriptStub(string language, string name)
    {
        return language == "lua"
            ? $"-- {name}.lua\nprint(\"Hello from {name}!\")\n"
            : $"# {name}.py\nprint(\"Hello from {name}!\")\n";
    }

    protected override async void OnClosing(Avalonia.Controls.WindowClosingEventArgs e)
    {
        base.OnClosing(e);
        if (e.Cancel) return;

        if (DataContext is not MainWindowViewModel vm)
        {
            SaveDockLayout();
            return;
        }

        // Always persist the dock layout (tabs, side panels) on
        // close, regardless of project-dirty state — the layout is
        // a workspace preference, not a project artifact.
        SaveDockLayout();

        if (vm.Project is not { IsDirty: true } project) return;

        e.Cancel = true;
        var dlg = ConfirmDialog.Create(
            "Unsaved changes",
            $"Project '{project.Name}' has unsaved changes. Save before closing?");
        var result = await dlg.ShowDialogAsync(this);
        switch (result)
        {
            case ConfirmResult.Save:
                if (PrimalEditor_Avalonia.Common.EditorSaveService.Instance.SaveAll(project))
                {
                    e.Cancel = false;
                    Close();
                }
                else
                {
                    // SaveAll reported a partial failure. Project stays dirty;
                    // surface the failures in a follow-up dialog so the user
                    // knows what didn't make it to disk. The user can choose
                    // to discard-and-close, retry, or stay open.
                    var failText = PrimalEditor_Avalonia.Common.EditorSaveService.LastSaveAllFailures.Count == 0
                        ? "Save failed (no details logged)."
                        : string.Join("\n", PrimalEditor_Avalonia.Common.EditorSaveService.LastSaveAllFailures);
                    var failDlg = ConfirmDialog.Create(
                        "Save failed",
                        $"{failText}\n\nClose anyway (discard), or cancel and retry?");
                    var follow = await failDlg.ShowDialogAsync(this);
                    if (follow == ConfirmResult.Discard)
                    {
                        e.Cancel = false;
                        Close();
                    }
                    // else: stay open, project remains dirty.
                }
                break;
            case ConfirmResult.Discard:
                e.Cancel = false;
                Close();
                break;
            case ConfirmResult.Cancel:
            case null:
                break;
        }
    }

    private void SaveDockLayout()
    {
        if (DataContext is not MainWindowViewModel vm) return;
        var snapshot = EditorLayoutSettings.Load();
        snapshot.DockLayout = vm.CaptureLayoutForSave();
        EditorLayoutSettings.Save(snapshot);
    }
}