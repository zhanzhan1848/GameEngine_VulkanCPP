using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Data.Core;
using System.Linq;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.Importers;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views;

namespace PrimalEditor_Avalonia;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);

        Logger.Current = new ConsoleLogger();

        // Catch-all for managed exceptions that escape our async void event
        // handlers (palette drag, OnNodeDoubleTapped, etc.). Avalonia does
        // not wrap event dispatch in try/catch, so a single throw from an
        // event handler reaches RunMain and aborts the process. We log
        // here so the next crash has something actionable in the log instead
        // of just a SIGABRT stack ending in a JIT trampoline.
        System.AppDomain.CurrentDomain.UnhandledException += (s, e) =>
        {
            var ex = e.ExceptionObject as System.Exception;
            var callerStack = new System.Diagnostics.StackTrace(1, true).ToString();
            Logger.Error($"AppDomain unhandled exception (terminating={e.IsTerminating}): {ex?.Message}\n  CallerStack:\n{callerStack}", ex);
        };
        Avalonia.Threading.Dispatcher.UIThread.UnhandledException += (s, e) =>
        {
            var callerStack = new System.Diagnostics.StackTrace(1, true).ToString();
            Logger.Error($"Dispatcher unhandled exception: {e.Exception?.Message}\n  CallerStack:\n{callerStack}", e.Exception);
            e.Handled = true;
        };

        NativeEngineBridge.LoadEngine();
        if (NativeEngineBridge.IsLoaded)
        {
            NativeEngineBridge.InitializeAssetSystem();
            EngineTick.Start();
        }
        PrimalEditor_Avalonia.Common.AutoSaveService.Start();

        // CLI / Agent 命令通道（Phase 0）。内部全捕获：通道失败只记日志，
        // 编辑器照常启动。PRIMAL_CLI_DISABLE=1 可整体关闭。
        Cli.CliHost.Start();

        System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(TextureImporterPlugin).TypeHandle);
        System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(MeshImporterPlugin).TypeHandle);
        System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(PrimitiveMeshImporterPlugin).TypeHandle);
        Logger.Info($"AssetImporterRegistry: {AssetImporterRegistry.Plugins.Count} plugin(s) registered");

        EnsureAstcencAvailable();
    }

    /// <summary>
    /// ContentTools' Metal texture cook shells out to the external astcenc CLI
    /// (astcenc-neon). Point it at the binary before the first import: prefer
    /// an existing PRIMAL_ASTCENC_PATH, then probe common locations next to the
    /// editor output and in the repo's third_party checkout.
    /// </summary>
    private static void EnsureAstcencAvailable()
    {
        var resolved = AstcencLocator.EnsureExported();
        if (resolved == null)
        {
            Logger.Warning("astcenc-neon not found; texture compression via ContentTools will fail. Set PRIMAL_ASTCENC_PATH to the astcenc binary.");
            return;
        }
        Logger.Info($"astcenc resolved: {resolved}");
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            DisableAvaloniaDataAnnotationValidation();

            // Apply the persisted UI theme (default = App.axaml boot theme)
            // before any window is created so the first frame already has it.
            Themes.ThemeManager.Initialize();

            // Avalonia's StartCore auto-shows whatever is assigned to
            // desktop.MainWindow. To delay the main window until the
            // user picks a project, we keep desktop.MainWindow unset
            // and show the project browser first as a free-floating
            // top-level window (no owner — ShowDialog requires a
            // visible owner, which the main window isn't yet).
            //
            // ShutdownMode defaults to OnLastWindowClose, which is
            // exactly what we want: the browser is the only visible
            // window until the user picks a project, so cancelling
            // it auto-exits. After we ShowMain we close the browser
            // so the main window becomes the sole window, and
            // closing it also auto-exits the app and fires Exit
            // (which runs OnExit to release native resources).
            desktop.Exit += (_, _) => OnExit();

            ShowProjectBrowserThenMain(desktop);
        }

        base.OnFrameworkInitializationCompleted();
    }

    /// <summary>
    /// Show the project browser dialog before the main window. The
    /// browser lets the user open an existing project or create a
    /// new one. If the user cancels, the application exits (matching
    /// the WPF ProjectBrowserDialog behaviour). Otherwise the chosen
    /// project is assigned and the main window is shown.
    /// </summary>
    private static void ShowProjectBrowserThenMain(IClassicDesktopStyleApplicationLifetime desktop)
    {
        var savedLayout = EditorLayoutSettings.Load().DockLayout;
        var vm = new MainWindowViewModel();
        if (savedLayout != null) vm.ApplyLayout(savedLayout);

        var mainWindow = new MainWindow
        {
            DataContext = vm,
        };
        Cli.EditorServices.Attach(mainWindow, vm);

        // CI / Agent 冒烟通道（CLI 规划 §8）：PRIMAL_OPEN_PROJECT 指向 .primal 文件
        // 时跳过项目浏览器直接打开——无 GUI 交互的编辑器闭环冒烟靠它。失败时降级
        // 回浏览器流程，不影响人工启动。
        var envProject = System.Environment.GetEnvironmentVariable("PRIMAL_OPEN_PROJECT");
        if (!string.IsNullOrWhiteSpace(envProject))
        {
            try
            {
                var opened = OpenProject.Open(envProject);
                if (opened != null)
                {
                    vm.Project = opened;
                    Logger.Info($"PRIMAL_OPEN_PROJECT: opened {envProject}");
                    ShowMain(desktop, mainWindow, vm);
                    return;
                }
                Logger.Warning($"PRIMAL_OPEN_PROJECT: open failed ({envProject}); falling back to project browser.");
            }
            catch (System.Exception ex)
            {
                Logger.Warning($"PRIMAL_OPEN_PROJECT: {ex.Message}; falling back to project browser.");
            }
        }

        var browser = new ProjectBrowserView(new ProjectBrowserViewModel());
        browser.Closed += (_, _) => OnProjectBrowserClosed(desktop, browser, vm, mainWindow);
        browser.Show();
    }

    private static async void OnProjectBrowserClosed(
        IClassicDesktopStyleApplicationLifetime desktop,
        ProjectBrowserView browser,
        MainWindowViewModel vm,
        MainWindow mainWindow)
    {
        // async void: an unhandled exception here kills the process
        // (observed as SIGABRT inside the engine's exit-time free_list
        // assert, which masks the real error). Guard everything so a
        // failure degrades to a log line + the browser reopening.
        try
        {
            await OnProjectBrowserClosedCore(desktop, browser, vm, mainWindow);
        }
        catch (System.Exception ex)
        {
            Logger.Error("Project browser: open flow failed", ex);
            ShowMain(desktop, mainWindow, vm);
        }
    }

    private static async System.Threading.Tasks.Task OnProjectBrowserClosedCore(
        IClassicDesktopStyleApplicationLifetime desktop,
        ProjectBrowserView browser,
        MainWindowViewModel vm,
        MainWindow mainWindow)
    {
        switch (browser.Result)
        {
            case ProjectBrowserView.BrowserResult.OpenProject:
                var path = browser.SelectedProjectPath;
                if (path == null)
                {
                    ShowOpenError("No project file was selected.");
                    ReopenProjectBrowser(desktop, mainWindow, vm);
                    return;
                }
                var bundles = AutoSaveRecoveryService.DiscoverBundles(
                    System.IO.Path.GetDirectoryName(path) ?? string.Empty);
                var candidate = AutoSaveRecoveryService.SelectNewestEligible(path, bundles);
                if (candidate == null)
                {
                    var opened = OpenProject.Open(path);
                    if (opened == null)
                    {
                        // OpenProject.Open already logged the underlying
                        // cause (file not found, migration failed, manifest
                        // invalid). Surface a user-facing message and re-open
                        // the browser so the user can pick a different
                        // project. Returning here would leave the app with
                        // zero visible windows and Avalonia's
                        // OnLastWindowClose shutdown would exit the process.
                        ShowOpenError($"Could not open project:\n{path}");
                        ReopenProjectBrowser(desktop, mainWindow, vm);
                        return;
                    }
                    vm.Project = opened;
                }
                else
                {
                    // Show the main window BEFORE prompting: the recovery
                    // dialog is owned by it, and ShowDialog on a never-shown
                    // owner throws. Showing first also means a recovery
                    // failure still leaves the editor UI open.
                    // The browser owns recovery for this explicit project open.
                    // Suppress the Opened handler before Show starts its async scan.
                    mainWindow.SkipStartupRecoveryScan();
                    ShowMain(desktop, mainWindow, vm);
                    await mainWindow.PromptAndApplyRecoveryAsync(
                        new AutoSaveRecoveryCoordinator.RecoveryCandidate(candidate, path));
                }
                ShowMain(desktop, mainWindow, vm);
                break;

            case ProjectBrowserView.BrowserResult.NewProject:
                if (browser.CreatedProject != null)
                {
                    vm.Project = browser.CreatedProject;
                    ShowMain(desktop, mainWindow, vm);
                }
                break;

            default:
                // Cancelled or unrecognised: leave the browser closed.
                // OnLastWindowClose will auto-exit because no other
                // window is open yet.
                break;
        }
    }

    private static void ShowMain(
        IClassicDesktopStyleApplicationLifetime desktop,
        MainWindow mainWindow,
        MainWindowViewModel vm)
    {
        desktop.MainWindow = mainWindow;
        mainWindow.Show();
        mainWindow.WindowState = Avalonia.Controls.WindowState.Normal;
        mainWindow.Activate();
    }

    /// <summary>
    /// Show a non-fatal error to the user. We use <see cref="ILogger"/>
    /// rather than <c>ErrorDialog.ShowAsync</c> because at the point
    /// this is called the editor has no visible main window yet
    /// (MainWindow is shown only on the success path), and the dialog's
    /// owner-fallback resolves to <c>desktop.MainWindow</c> which is
    /// still null. The Logger output reaches the in-process console
    /// (visible when launched from a terminal) and the optional
    /// <c>EditorLog.txt</c> sink in the user's project directory.
    /// </summary>
    private static void ShowOpenError(string message)
    {
        Logger.Error(message);
    }

    /// <summary>
    /// Open the project browser again so the user can pick a different
    /// project after a load failure. Necessary because the previous
    /// browser was closed (the only window) when its OnOpenSelected /
    /// OnNewCreated handler fired — without this, the user would
    /// be staring at a frozen "Engine tick stopped" state with no way
    /// to recover.
    /// </summary>
    private static void ReopenProjectBrowser(
        IClassicDesktopStyleApplicationLifetime desktop,
        MainWindow mainWindow,
        MainWindowViewModel vm)
    {
        var browser = new ProjectBrowserView(new ProjectBrowserViewModel());
        browser.Closed += (_, _) => OnProjectBrowserClosed(desktop, browser, vm, mainWindow);
        browser.Show();
    }

    private void OnExit()
    {
        // Workaround: the native engine's JobSystem workers still hold
        // ResourceManager locks for ~1s after ShutdownEngine returns;
        // calling UnloadEngine synchronously while they run causes a
        // pthread mutex deadlock (SIGABRT). A small delay lets the
        // workers drain before the library is freed.
        Cli.CliHost.Stop(); // 先关命令通道：退出期间不再接受新命令
        PrimalEditor_Avalonia.Common.AutoSaveService.Stop();
        EngineTick.Stop();
        if (NativeEngineBridge.IsLoaded)
        {
            // Unregister mesh entities while the render pipeline and content
            // system still exist. A late layout/init retry must never attempt
            // PipelineRegisterMeshEntity after these systems begin shutdown.
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop &&
                desktop.MainWindow?.DataContext is MainWindowViewModel mainVm)
            {
                try { mainVm.WorldEditor.SceneSynchronizer.Teardown(); } catch { }
            }
            foreach (var loopId in PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D.LiveNativeLoopIds.ToArray())
            {
                try { NativeEngineBridge.EditorRenderLoop.Destroy(loopId); } catch { }
            }
            try { NativeEngineBridge.FlushGPU(); } catch { }
            try { NativeEngineBridge.DestroyStandardRenderPipeline(); } catch { }
            // 退出兜底（OnDetached 注释里约定的"App.OnExit 清理"）：会话期间
            // 表面被有意保留，引擎 ShutdownEngine 不清表面/窗口注册表，遗留
            // 条目会在静态析构阶段触发 free_list 泄漏哨兵断言（FreeList.h:27，
            // SIGABRT）。必须在 ShutdownEngine 前移除。
            try { NativeEngineBridge.RemoveAllRenderSurfaces(); } catch { }
            try { NativeEngineBridge.ShutdownAssetSystem(); } catch { }
            try { NativeEngineBridge.ShutdownEngine(); } catch { }
            try { System.Threading.Thread.Sleep(1500); } catch { }
            try { NativeEngineBridge.UnloadEngine(); } catch { }
        }
    }

    private void DisableAvaloniaDataAnnotationValidation()
    {
        // Avalonia 12 removed the Avalonia.Data.Core.Plugins.DataAnnotationsValidationPlugin
        // binding pipeline; data annotation validation is no longer active, so the
        // Avalonia 11 disable hook is now a no-op. Keep the method (called from
        // OnFrameworkInitializationCompleted) for API stability with downstream tooling.
    }
}
