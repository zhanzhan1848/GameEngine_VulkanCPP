using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;

namespace PrimalEditor_Avalonia.Views;

public partial class RenameEntityDialog : Window
{
    public RenameEntityDialog()
    {
        InitializeComponent();
    }

    public RenameEntityDialog(string currentName) : this()
    {
        NameTextBox.Text = currentName;
        Dispatcher.UIThread.Post(() => NameTextBox.SelectAll());
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnCancel(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close(null);
    }

    private void OnOK(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close(NameTextBox.Text);
    }
}