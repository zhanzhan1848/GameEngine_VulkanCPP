using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class OpenProjectView : Window
{
    public OpenProjectView()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnBrowseClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            vm.BrowseForFile();
        }
    }

    private void OnRemoveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            vm.RemoveSelected();
        }
    }

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close(null);
    }

    private void OnOpenClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            var result = vm.OpenSelectedProject();
            if (result != null)
            {
                Close(result);
            }
        }
    }

    private void OnProjectDoubleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            var result = vm.OpenSelectedProject();
            if (result != null)
            {
                Close(result);
            }
        }
    }
}