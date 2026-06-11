using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

public partial class AboutDialog : Window
{
    public AboutDialog()
    {
        InitializeComponent();
        var runtimeVersion = this.FindControl<TextBlock>("RuntimeVersion");
        var osVersion = this.FindControl<TextBlock>("OsVersion");
        if (runtimeVersion != null)
            runtimeVersion.Text = System.Environment.Version.ToString();
        if (osVersion != null)
            osVersion.Text = $"{System.Environment.OSVersion.Platform} {System.Environment.OSVersion.Version}";
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnOkClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close();
    }
}