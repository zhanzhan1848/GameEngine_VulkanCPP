using System.IO;
using System.Linq;
using Avalonia.Controls;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class MainWindow : Window
{
    private bool _reopenAttempted;
    private bool _recoveryScanned;
    // Set once the user answered the unsaved-changes prompt with
    // save/discard so the follow-up Close() skips the dirty check.
    // Without it, Close() from inside OnClosing re-enters this
    // handler and re-prompts forever.
    private bool _confirmedClose;
    private readonly System.Threading.SemaphoreSlim _recoveryPromptGate = new(1, 1);

    public MainWindow()
    {
        InitializeComponent();
        Opened += OnMainWindowOpened;
        InitializeThemeMenu();


    }

    /// <summary>
    /// Build the View ▸ Theme submenu from <see cref="Themes.EditorThemeRegistry.All"/>
    /// (radio items) and keep the checks in sync with <see cref="Themes.ThemeManager"/>.
    /// </summary>
    private void InitializeThemeMenu()
    {
        foreach (var theme in Themes.EditorThemeRegistry.All)
        {
            var item = new MenuItem
            {
                Header = "_" + theme.DisplayName,
                Tag = theme.Id,
                ToggleType = MenuItemToggleType.Radio,
                IsChecked = Themes.ThemeManager.Current.Id == theme.Id,
            };
            item.Click += OnThemeMenuItemClick;
            ThemeMenu.Items.Add(item);
        }
        Themes.ThemeManager.ThemeChanged += OnThemeManagerChanged;
        Closed += (_, _) => Themes.ThemeManager.ThemeChanged -= OnThemeManagerChanged;
    }

    private void OnThemeMenuItemClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is MenuItem { Tag: string themeId })
        {
            Themes.ThemeManager.Apply(themeId);
        }
    }

    private void OnThemeManagerChanged(Themes.EditorTheme theme)
    {
        foreach (var item in ThemeMenu.Items.OfType<MenuItem>())
        {
            item.IsChecked = item.Tag as string == theme.Id;
        }
    }

    /// <summary>
    /// Restore the asset windows that were open at the end of the
    /// previous editor session, as recorded by
    /// <see cref="EditorLayoutSettings"/>. Runs once per process to
    /// avoid re-opening on every layout pass.
    /// </summary>
    private async void OnMainWindowOpened(object? sender, System.EventArgs e)
    {
        if (_reopenAttempted) return;
        _reopenAttempted = true;
        try
        {
            ReopenLastSessionAssets();
        }
        catch (System.Exception ex)
        {
            Logger.Error($"MainWindow: failed to reopen last session: {ex.Message}\n{ex.StackTrace}", ex);
        }
        await ScanForRecoveryOnceAsync();
    }

    /// <summary>
    /// Scan recent projects for the newest eligible autosave
    /// recovery candidate. Runs at most once per MainWindow
    /// instance. The dialog itself only runs on the UI thread
    /// (via the caller's awaited promise). Skipped entirely if a
    /// project has already been assigned (e.g. by an earlier
    /// recovery or programmatic load).
    /// </summary>
    internal void SkipStartupRecoveryScan() => _recoveryScanned = true;

    public async System.Threading.Tasks.Task ScanForRecoveryOnceAsync()
    {
        if (_recoveryScanned) return;
        if (DataContext is MainWindowViewModel alreadyAssigned && alreadyAssigned.Project != null) return;
        _recoveryScanned = true;
        try
        {
            var recent = new RecentProjectsManager();
            recent.Load();
            if (recent.RecentProjects.Count == 0) return;
            var coordinator = new AutoSaveRecoveryCoordinator(recent.RecentProjects);
            AutoSaveRecoveryCoordinator.RecoveryCandidate? candidate;
            try
            {
                candidate = await System.Threading.Tasks.Task.Run(
                    () => coordinator.ScanForRecoveryCandidates());
            }
            catch (System.Exception ex)
            {
                Logger.Error("MainWindow: recovery coordinator failed", ex);
                return;
            }
            if (candidate == null) return;
            if (DataContext is MainWindowViewModel vm && vm.Project != null) return;
            await PromptAndApplyRecoveryAsync(candidate);
        }
        catch (System.Exception ex)
        {
            Logger.Error("MainWindow: recovery scan failed", ex);
        }
    }

    /// <summary>
    /// Show the recovery dialog and apply the user's decision. Called
    /// from both the startup scan and the explicit project-open path.
    /// </summary>
    public async System.Threading.Tasks.Task PromptAndApplyRecoveryAsync(
        AutoSaveRecoveryCoordinator.RecoveryCandidate candidate)
    {
        if (candidate == null) return;
        await _recoveryPromptGate.WaitAsync();
        try
        {
            if (DataContext is not MainWindowViewModel vm) return;

            var dialog = new RecoverAutosaveDialog(
                candidate.Bundle.ProjectName,
                candidate.Bundle.Timestamp,
                candidate.Bundle.BundlePath,
                candidate.OriginalProjectFilePath);

            var decision = await dialog.ShowDialog<AutoSaveRecoveryDecision?>(this);
            switch (decision)
            {
                case AutoSaveRecoveryDecision.RecoverAutosave:
                {
                    var loaded = AutoSaveRecoveryService.LoadBundle(
                        candidate.Bundle, candidate.OriginalProjectFilePath);
                    if (loaded == null)
                    {
                        Logger.Error($"AutoSave recovery: failed to load bundle {candidate.Bundle.BundlePath}");
                        return;
                    }
                    vm.AssignProject(loaded, markRecoveredDirty: true);
                    break;
                }
                case AutoSaveRecoveryDecision.OpenSavedVersion:
                {
                    var loadedLive = OpenProject.Open(candidate.OriginalProjectFilePath);
                    if (loadedLive == null)
                    {
                        Logger.Error($"AutoSave recovery: failed to load live project for '{candidate.OriginalProjectFilePath}'; backup left eligible.");
                        return;
                    }
                    AutoSaveRecoveryService.MarkDismissed(candidate.Bundle);
                    vm.Project = loadedLive;
                    break;
                }
                case AutoSaveRecoveryDecision.Later:
                default:
                {
                    // Deferred: still open the live project so the editor
                    // isn't left project-less; the bundle stays eligible
                    // for a later recovery prompt.
                    var loadedLive = OpenProject.Open(candidate.OriginalProjectFilePath);
                    if (loadedLive != null) vm.Project = loadedLive;
                    break;
                }
            }
        }
        finally
        {
            _recoveryPromptGate.Release();
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

        if (_confirmedClose || vm.Project is not { IsDirty: true } project) return;

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
                    _confirmedClose = true;
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
                        _confirmedClose = true;
                        Close();
                    }
                    // else: stay open, project remains dirty.
                }
                break;
            case ConfirmResult.Discard:
                _confirmedClose = true;
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

    /// <summary>
    /// Tear down every open asset editor when the main window goes away.
    /// Without this, Avalonia's default ShutdownMode.OnLastWindowClose
    /// keeps the process alive while any AssetEditorHost window is still
    /// open. Closing them here also runs each host's OnClosed, which
    /// strips its path from editor-layout.json's OpenAssets list so
    /// the next session doesn't auto-reopen closed assets.
    /// </summary>
    protected override void OnClosed(System.EventArgs e)
    {
        foreach (var host in AssetEditorRegistry.OpenHosts.ToArray())
        {
            try { host.Close(); }
            catch (System.Exception ex)
            {
                Logger.Error($"MainWindow: failed to close asset editor '{host.ViewModel?.Asset?.FileName}'", ex);
            }
        }
        base.OnClosed(e);
    }
}
