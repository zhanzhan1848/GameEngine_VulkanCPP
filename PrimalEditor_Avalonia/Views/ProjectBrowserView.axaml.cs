using System;
using Avalonia.Controls;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class ProjectBrowserView : Window
{
    public enum BrowserResult { OpenProject, NewProject, Cancelled }

    public BrowserResult Result { get; private set; } = BrowserResult.Cancelled;
    public Project? CreatedProject { get; private set; }
    public string? SelectedProjectPath { get; private set; }

    public ProjectBrowserView()
    {
        InitializeComponent();
    }

    public ProjectBrowserView(ProjectBrowserViewModel vm) : this()
    {
        DataContext = vm;
        openPanel.DataContext = vm.OpenProject;
        newPanel.DataContext = vm.NewProject;
        openPanel.ProjectSelected += OnOpenSelected;
        openPanel.Cancelled += OnCancelled;
        newPanel.ProjectCreated += OnNewCreated;
        newPanel.Cancelled += OnCancelled;
    }

    private void OnOpenTabClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        openPanel.IsVisible = true;
        newPanel.IsVisible = false;
        openProjectButton.IsChecked = true;
        createProjectButton.IsChecked = false;
    }

    private void OnCreateTabClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        openPanel.IsVisible = false;
        newPanel.IsVisible = true;
        openProjectButton.IsChecked = false;
        createProjectButton.IsChecked = true;
    }

    private void OnOpenSelected(object? sender, string path)
    {
        SelectedProjectPath = path;
        Result = BrowserResult.OpenProject;
        Close();
    }

    private void OnNewCreated(object? sender, Project project)
    {
        CreatedProject = project;
        Result = BrowserResult.NewProject;
        Close(project);
    }

    private void OnCancelled(object? sender, EventArgs e)
    {
        Result = BrowserResult.Cancelled;
        Close();
    }
}