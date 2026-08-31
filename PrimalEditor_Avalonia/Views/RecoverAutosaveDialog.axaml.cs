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
        // Calls the source-generated InitializeComponent(bool, bool) — which
        // loads the XAML AND assigns the named-control fields below. Do NOT
        // reintroduce a hand-written parameterless InitializeComponent here:
        // it shadows the generated one and leaves ProjectNameText etc. null
        // (NullReferenceException in the 4-arg constructor).
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

    private void OnRecoverClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.RecoverAutosave);

    private void OnOpenSavedClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.OpenSavedVersion);

    private void OnLaterClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
        => Close(AutoSaveRecoveryDecision.Later);
}