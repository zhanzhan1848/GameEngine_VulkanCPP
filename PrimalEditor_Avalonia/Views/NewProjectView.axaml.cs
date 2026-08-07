using System;
using Avalonia.Controls;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class NewProjectView : UserControl
{
    public event EventHandler<Project?>? ProjectCreated;
    public event EventHandler? Cancelled;

    public NewProjectView()
    {
        InitializeComponent();
    }

    private void OnBrowseClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is NewProjectViewModel vm)
        {
            vm.BrowseForPath();
        }
    }

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Cancelled?.Invoke(this, EventArgs.Empty);
    }

    private void OnCreateClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is NewProjectViewModel vm)
        {
            var result = vm.CreateProject();
            if (result != null)
            {
                ProjectCreated?.Invoke(this, result);
            }
        }
    }
}