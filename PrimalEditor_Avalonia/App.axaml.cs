using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Data.Core;
using Avalonia.Data.Core.Plugins;
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
        NativeEngineBridge.LoadEngine();
        if (NativeEngineBridge.IsLoaded)
        {
            NativeEngineBridge.InitializeAssetSystem();
            EngineTick.Start();
        }
        PrimalEditor_Avalonia.Common.AutoSaveService.Start();

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
        PrimalEditor_Avalonia.Common.AutoSaveService.Stop();
        EngineTick.Stop();
        if (NativeEngineBridge.IsLoaded)
        {
            foreach (var loopId in PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D.LiveNativeLoopIds.ToArray())
            {
                try { NativeEngineBridge.EditorRenderLoop.Destroy(loopId); } catch { }
            }
            try { NativeEngineBridge.FlushGPU(); } catch { }
            try { NativeEngineBridge.DestroyStandardRenderPipeline(); } catch { }
            try { NativeEngineBridge.ShutdownAssetSystem(); } catch { }
            try { NativeEngineBridge.ShutdownEngine(); } catch { }
            try { System.Threading.Thread.Sleep(1500); } catch { }
            try { NativeEngineBridge.UnloadEngine(); } catch { }
        }
    }

    private void DisableAvaloniaDataAnnotationValidation()
    {
        var dataValidationPluginsToRemove =
            BindingPlugins.DataValidators.OfType<DataAnnotationsValidationPlugin>().ToArray();
        foreach (var plugin in dataValidationPluginsToRemove)
        {
            BindingPlugins.DataValidators.Remove(plugin);
        }
    }
}