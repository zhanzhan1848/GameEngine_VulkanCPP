using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

public partial class AboutDialog : Window
{
    public AboutDialog()
    {
        InitializeComponent();
        RuntimeVersion.Text = System.Environment.Version.ToString();
        OsVersion.Text = $"{System.Environment.OSVersion.Platform} {System.Environment.OSVersion.Version}";
    }

    private void OnOkClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close();
    }
}