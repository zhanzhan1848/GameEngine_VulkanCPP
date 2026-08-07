using System;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views;

public partial class RecoverAutosaveDialog : Window
{
    public string ProjectName { get; }
    public string BackupTimestampDisplay { get; }
    public string BundlePath { get; }
    public string OriginalProjectPath { get; }

    public RecoverAutosaveDialog()
    {
        InitializeComponent();
        ProjectName = string.Empty;
        BackupTimestampDisplay = string.Empty;
        BundlePath = string.Empty;
        OriginalProjectPath = string.Empty;
        DataContext = this;
    }

    public RecoverAutosaveDialog(string projectName, DateTime backupTimestamp,
        string bundlePath, string originalProjectPath) : this()
    {
        ProjectName = projectName;
        BackupTimestampDisplay = backupTimestamp.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss");
        BundlePath = bundlePath;
        OriginalProjectPath = originalProjectPath;
        DataContext = this;
        ProjectNameText.Text = projectName;
        BackupTimeText.Text = BackupTimestampDisplay;
        BundlePathText.Text = bundlePath;
        OriginalPathText.Text = originalProjectPath;
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnRecoverClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.RecoverAutosave);

    private void OnOpenSavedClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.OpenSavedVersion);

    private void OnLaterClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.Later);
}