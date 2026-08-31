using System;
using Avalonia.Controls;
using PrimalEditor_Avalonia.GameProject;
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

    private async void OnBrowseClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is OpenProjectViewModel vm)
        {
            await vm.BrowseForFileAsync(TopLevel.GetTopLevel(this));
        }
    }

    private async void OnRemoveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not OpenProjectViewModel vm) return;
        if (vm.SelectedProject == null) return;
        if (TopLevel.GetTopLevel(this) is not Window owner) return;

        var root = DeleteProject.ResolveProjectRoot(vm.SelectedProject.Path);
        var missing = root == null;
        var message = missing
            ? $"'{vm.SelectedProject.Name}' was not found on disk. Remove it from the recent projects list?"
            : $"Delete '{vm.SelectedProject.Name}' and everything in it?\n\n{root}\n\n" +
              "The entire project folder will be permanently deleted. This cannot be undone.";

        var dlg = ConfirmDialog.CreateDestructive(
            "Remove project", message, missing ? "Remove" : "Delete");
        var result = await dlg.ShowDialogAsync(owner);
        if (result != ConfirmResult.Discard) return;

        vm.RemoveSelected();
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