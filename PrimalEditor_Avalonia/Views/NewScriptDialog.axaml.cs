using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

public partial class NewScriptDialog : Window
{
    public NewScriptDialog()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    public sealed class Result
    {
        public string Name { get; set; } = string.Empty;
        public string Language { get; set; } = "python";
    }

    public async System.Threading.Tasks.Task<Result?> ShowDialogAsync(Window owner)
    {
        var nameBox = this.FindControl<TextBox>("NameBox");
        if (nameBox != null) nameBox.Focus();
        return await ShowDialog<Result?>(owner);
    }

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(null);

    private void OnCreateClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var nameBox = this.FindControl<TextBox>("NameBox");
        var langBox = this.FindControl<ComboBox>("LanguageBox");
        var name = nameBox?.Text?.Trim() ?? string.Empty;
        if (string.IsNullOrEmpty(name)) { Close(null); return; }
        var idx = langBox?.SelectedIndex ?? 0;
        var lang = idx == 1 ? "lua" : "python";
        Close(new Result { Name = name, Language = lang });
    }
}
