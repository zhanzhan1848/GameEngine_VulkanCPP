using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Common;

public enum ErrorSeverity
{
    Info,
    Warning,
    Error,
    Critical,
}

public static class ErrorDialog
{
    public static async Task ShowAsync(string title, string message, ErrorSeverity severity = ErrorSeverity.Error)
    {
        var topLevel = GetTopLevel();
        if (topLevel == null)
        {
            Logger.Error($"[{severity}] {title}: {message}");
            return;
        }

        var dialog = new Window
        {
            Title = title,
            Width = 500,
            SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            CanResize = false,
        };

        var icon = severity switch
        {
            ErrorSeverity.Info => "ℹ️",
            ErrorSeverity.Warning => "⚠️",
            ErrorSeverity.Error => "❌",
            ErrorSeverity.Critical => "🚨",
        };
        var color = severity switch
        {
            ErrorSeverity.Info => "#1E88E5",
            ErrorSeverity.Warning => "#FB8C00",
            ErrorSeverity.Error => "#E53935",
            ErrorSeverity.Critical => "#B71C1C",
        };

        var textBlock = new TextBlock
        {
            Text = $"{icon} {message}",
            TextWrapping = Avalonia.Media.TextWrapping.Wrap,
            Foreground = new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.Parse(color)),
            Margin = new Avalonia.Thickness(20),
            MaxWidth = 460,
        };

        var okButton = new Button
        {
            Content = "OK",
            Width = 80,
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center,
            Margin = new Avalonia.Thickness(0, 10, 0, 20),
        };
        okButton.Click += (_, _) => dialog.Close();

        var panel = new DockPanel();
        DockPanel.SetDock(okButton, Dock.Bottom);
        panel.Children.Add(okButton);
        panel.Children.Add(textBlock);

        dialog.Content = panel;

        if (topLevel is Window owner)
        {
            await dialog.ShowDialog(owner);
        }
        else
        {
            dialog.Show();
        }

        Logger.Error($"[{severity}] {title}: {message}");
    }

    private static TopLevel? GetTopLevel()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            return desktop.MainWindow;
        }
        return null;
    }
}