using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Data.Core;
using Avalonia.Data.Core.Plugins;
using System.Linq;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;
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
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            DisableAvaloniaDataAnnotationValidation();
            desktop.MainWindow = new MainWindow
            {
                DataContext = new MainWindowViewModel(),
            };
            desktop.MainWindow.Show();
            desktop.MainWindow.WindowState = Avalonia.Controls.WindowState.Normal;
            desktop.MainWindow.Activate();
            desktop.Exit += (_, _) => OnExit();
        }

        base.OnFrameworkInitializationCompleted();
    }

    private void OnExit()
    {
        PrimalEditor_Avalonia.Common.AutoSaveService.Stop();
        EngineTick.Stop();
        if (NativeEngineBridge.IsLoaded)
        {
            // Tear down in the documented order: render loop first (so
            // the CFRunLoop timer stops firing on the AppKit main thread
            // before we begin dismantling the engine), then the render
            // pipeline, then any embedded render surfaces (so the MTKView
            // detaches cleanly), then the engine itself, then the asset
            // system. Doing these in the wrong order either leaks
            // GPU/renderer objects (dylib unloads with Metal still alive)
            // or tears down the asset system before the renderer that
            // consumes it.
            //
            // EditorRenderLoop.Stop(0)/Destroy(0) are deliberately no-ops;
            // we have to iterate the live loop ids returned by
            // EngineViewport3D.LiveNativeLoopIds and Destroy() each one.
            // We snapshot the set before iterating because Destroy()s
            // remove from the set as a side effect.
            foreach (var loopId in PrimalEditor_Avalonia.Views.EngineViewport3D.LiveNativeLoopIds.ToArray())
            {
                try { NativeEngineBridge.EditorRenderLoop.Destroy(loopId); } catch { /* tolerate per-loop failure */ }
            }
            // Embedded surfaces owned by this process — best-effort removal.
            // The list isn't tracked here; future cleanup pass can wire a
            // dedicated ShutdownAllSurfaces P/Invoke if needed.
            try { NativeEngineBridge.DestroyStandardRenderPipeline(); } catch { }
            try { NativeEngineBridge.ShutdownEngine(); } catch { }
            try { NativeEngineBridge.ShutdownAssetSystem(); } catch { }
            NativeEngineBridge.UnloadEngine();
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