using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views;

public partial class BuildLogView : UserControl
{
    public BuildLogView()
    {
        InitializeComponent();
        BuildService.OnOutput += OnBuildOutput;
        BuildService.OnCompleted += OnBuildCompleted;
        Unloaded += (_, _) =>
        {
            BuildService.OnOutput -= OnBuildOutput;
            BuildService.OnCompleted -= OnBuildCompleted;
        };
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnBuildOutput(string line)
        => Avalonia.Threading.Dispatcher.UIThread.Post(() => AppendLine(line));

    private void OnBuildCompleted(int exitCode, bool success)
        => Avalonia.Threading.Dispatcher.UIThread.Post(() =>
            AppendLine($"[build] exit code={exitCode} success={success}"));

    private void AppendLine(string line)
    {
        if (this.FindControl<TextBox>("BuildLogText") is not TextBox textBox) return;
        textBox.Text += (textBox.Text.Length > 0 ? "\n" : string.Empty) + line;
    }

    private void OnBuildLogDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (this.FindControl<TextBox>("BuildLogText") is not TextBox logBox) return;
        var selected = logBox.SelectedText;
        if (string.IsNullOrEmpty(selected)) selected = logBox.Text ?? string.Empty;
        var hit = BuildErrorParser.Parse(selected);
        if (hit == null)
        {
            AppendLine("[jump] no file:line:col pattern in selection");
            return;
        }
        AppendLine($"[jump] {hit.Value.File}:{hit.Value.Line}:{hit.Value.Column} (file exists: {System.IO.File.Exists(hit.Value.File)})");
    }
}