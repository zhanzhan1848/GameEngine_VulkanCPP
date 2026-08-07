using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

public partial class TextInputDialog : Window
{
    public TextInputDialog() : this("Input", "Enter value:", string.Empty) { }

    public TextInputDialog(string title, string prompt, string initial)
    {
        InitializeComponent();
        Title = title;
        PromptText.Text = prompt;
        InputBox.Text = initial;
    }

    public async System.Threading.Tasks.Task<string?> ShowDialogAsync(Window owner)
    {
        InputBox.Focus();
        return await ShowDialog<string?>(owner);
    }

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(null);

    private void OnOkClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(InputBox.Text);
}
