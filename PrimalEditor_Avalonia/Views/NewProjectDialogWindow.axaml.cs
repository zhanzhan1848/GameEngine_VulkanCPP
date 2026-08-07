using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class NewProjectDialogWindow : Window
{
    private NewProjectView? _panel;

    public NewProjectDialogWindow()
    {
        InitializeComponent();
    }

    public NewProjectDialogWindow(NewProjectViewModel vm) : this()
    {
        _panel = new NewProjectView { DataContext = vm };
        _panel.ProjectCreated += (_, project) => Close(project);
        _panel.Cancelled += (_, _) => Close(null);
        Content = _panel;
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }
}