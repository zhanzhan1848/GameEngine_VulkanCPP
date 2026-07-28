using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public partial class PrimitiveMeshView : Window
{
    public PrimitiveMeshView()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => Close(false);

    private void OnCreateClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PrimitiveMeshViewModel vm) { Close(false); return; }

        var project = ActiveProject();
        if (project == null)
        {
            Logger.Warning("Primitive Mesh: no project loaded — closing dialog");
            Close(false);
            return;
        }

        var ok = vm.Create(project.ContentPath);
        Close(ok);
    }

    private static GameProject.Project? ActiveProject()
    {
        return Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is ViewModels.MainWindowViewModel main
            ? main.Project
            : null;
    }
}