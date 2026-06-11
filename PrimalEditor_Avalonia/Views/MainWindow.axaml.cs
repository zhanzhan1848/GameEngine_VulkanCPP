using Avalonia.Controls;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
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
}