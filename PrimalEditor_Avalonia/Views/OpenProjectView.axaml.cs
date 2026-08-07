using System;
using Avalonia.Controls;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class OpenProjectView : UserControl
{
    public event EventHandler<string?>? ProjectSelected;
    public event EventHandler? Cancelled;

    public OpenProjectView()
    {
        InitializeComponent();
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
        Cancelled?.Invoke(this, EventArgs.Empty);
    }

    private void OnOpenClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            var path = vm.GetSelectedProjectPath();
            if (path != null)
            {
                ProjectSelected?.Invoke(this, path);
            }
        }
    }

    private void OnProjectDoubleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            var path = vm.GetSelectedProjectPath();
            if (path != null)
            {
                ProjectSelected?.Invoke(this, path);
            }
        }
    }
}