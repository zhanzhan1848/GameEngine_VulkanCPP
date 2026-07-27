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
            // Teardown order is critical to avoid AutoreleasePoolPage::busted
            // and the EXC_BAD_ACCESS-in-presentDrawable crashes documented in
            // Engine/Graphics/Metal/MetalCore.cpp and tasks/lessons.md.
            //
            //   1. Stop every render loop (CFRunLoopTimer must not fire again).
            //   2. Block new frames from entering the engine.
            //   3. FlushGPU: wait for in-flight command buffers to complete.
            //      Without this, Metal's completion-queue dispatch can fire
            //      a presentDrawable: block AFTER the MTKView it captured has
            //      been deallocated by AppKit.
            //   4. Destroy the render pipeline (releases GPU-bound resources).
            //   5. ShutdownAssetSystem (releases file-backed assets).
            //   6. ShutdownEngine (releases Metal device + RHI).
            //   7. UnloadEngine (dlclose the dylib).
            //
            // EditorRenderLoop.Stop(0)/Destroy(0) are deliberately no-ops;
            // we iterate the live loop ids returned by
            // EngineViewport3D.LiveNativeLoopIds and Destroy() each one. We
            // snapshot the set before iterating because Destroy()s remove
            // from the set as a side effect.
            foreach (var loopId in PrimalEditor_Avalonia.Views.EngineViewport3D.LiveNativeLoopIds.ToArray())
            {
                try { NativeEngineBridge.EditorRenderLoop.Destroy(loopId); } catch { /* tolerate per-loop failure */ }
            }

            // Wait for the GPU to finish any in-flight work before tearing
            // down anything that might still be in use.
            try { NativeEngineBridge.FlushGPU(); } catch { /* tolerate; device may already be down */ }

            try { NativeEngineBridge.DestroyStandardRenderPipeline(); } catch { }
            try { NativeEngineBridge.ShutdownAssetSystem(); } catch { }
            try { NativeEngineBridge.ShutdownEngine(); } catch { }
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