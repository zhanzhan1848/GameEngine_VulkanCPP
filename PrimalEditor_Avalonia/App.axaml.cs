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
        Styles.Add(new Avalonia.Markup.Xaml.Styling.StyleInclude(
            new System.Uri("avares:PrimalEditor_Avalonia/Controls/Controls.axaml")));

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
            foreach (var loopId in PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D.LiveNativeLoopIds.ToArray())
            {
                try { NativeEngineBridge.EditorRenderLoop.Destroy(loopId); } catch { }
            }
            try { NativeEngineBridge.FlushGPU(); } catch { }
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