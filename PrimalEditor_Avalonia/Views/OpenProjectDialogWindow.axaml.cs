using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class OpenProjectDialogWindow : Window
{
    public static readonly StyledProperty<string?> ResultPathProperty =
        AvaloniaProperty.Register<OpenProjectDialogWindow, string?>(nameof(ResultPath));

    public string? ResultPath
    {
        get => GetValue(ResultPathProperty);
        private set => SetValue(ResultPathProperty, value);
    }

    private OpenProjectView? _panel;

    public OpenProjectDialogWindow()
    {
        InitializeComponent();
    }

    public OpenProjectDialogWindow(OpenProjectViewModel vm) : this()
    {
        AttachPanel(vm);
    }

    private void AttachPanel(OpenProjectViewModel vm)
    {
        _panel = new OpenProjectView { DataContext = vm };
        _panel.ProjectSelected += (_, path) =>
        {
            ResultPath = path;
            Close(path);
        };
        _panel.Cancelled += (_, _) =>
        {
            ResultPath = null;
            Close(null);
        };
        Content = _panel;
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }
}