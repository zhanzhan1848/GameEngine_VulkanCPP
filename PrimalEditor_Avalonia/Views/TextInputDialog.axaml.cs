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
        var promptText = this.FindControl<TextBlock>("PromptText");
        if (promptText != null) promptText.Text = prompt;
        var inputBox = this.FindControl<TextBox>("InputBox");
        if (inputBox != null) inputBox.Text = initial;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    public new string? Title
    {
        get => base.Title;
        set
        {
            base.Title = value ?? string.Empty;
            var promptText = this.FindControl<TextBlock>("PromptText");
            if (promptText != null) promptText.Text = value;
        }
    }

    public async System.Threading.Tasks.Task<string?> ShowDialogAsync(Window owner)
    {
        var inputBox = this.FindControl<TextBox>("InputBox");
        if (inputBox != null) inputBox.Focus();
        return await ShowDialog<string?>(owner);
    }

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(null);

    private void OnOkClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var inputBox = this.FindControl<TextBox>("InputBox");
        Close(inputBox?.Text);
    }
}
