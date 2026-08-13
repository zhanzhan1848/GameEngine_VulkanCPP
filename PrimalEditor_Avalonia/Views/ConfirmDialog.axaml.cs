using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

public enum ConfirmResult
{
    Save,
    Discard,
    Cancel,
}

public partial class ConfirmDialog : Window
{
    public ConfirmDialog()
    {
        InitializeComponent();
    }

    public static ConfirmDialog Create(string title, string message)
    {
        var dlg = new ConfirmDialog();
        dlg.Title = title;
        dlg.MessageText.Text = message;
        return dlg;
    }

    /// <summary>
    /// A two-button confirm/cancel dialog for destructive actions, where
    /// the three-way save/discard/cancel wording would be nonsense.
    /// Confirming returns <see cref="ConfirmResult.Discard"/>.
    /// </summary>
    public static ConfirmDialog CreateDestructive(string title, string message, string confirmText)
    {
        var dlg = Create(title, message);
        dlg.SaveButton.IsVisible = false;
        dlg.DiscardButton.Content = confirmText;
        dlg.DiscardButton.IsDefault = true;
        return dlg;
    }

    public async System.Threading.Tasks.Task<ConfirmResult?> ShowDialogAsync(Window owner)
    {
        return await ShowDialogCoreAsync(owner);
    }

    private async System.Threading.Tasks.Task<ConfirmResult?> ShowDialogCoreAsync(Window owner)
    {
        return await base.ShowDialog<ConfirmResult?>(owner);
    }

    private void OnSaveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        Close(ConfirmResult.Save);

    private void OnDiscardClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        Close(ConfirmResult.Discard);

    private void OnCancelClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        Close(ConfirmResult.Cancel);
}
