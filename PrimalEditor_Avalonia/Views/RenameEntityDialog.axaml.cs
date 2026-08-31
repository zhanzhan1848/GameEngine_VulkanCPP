using Avalonia.Controls;
using Avalonia.Threading;

namespace PrimalEditor_Avalonia.Views;

public partial class RenameEntityDialog : Window
{
    public RenameEntityDialog()
    {
        // Avalonia's source generator provides InitializeComponent(bool,
        // bool). Calling it here loads the XAML AND populates every
        // x:Name field on the partial class (NameTextBox, etc.). Do NOT
        // add a private InitializeComponent() override — that would
        // shadow the generator's version and the x:Name fields would
        // stay null. Symptom: NullReferenceException at the first field
        // access in the constructor (see WorldEditorView.OnEntityDoubleClick).
        InitializeComponent();
    }

    public RenameEntityDialog(string currentName) : this()
    {
        NameTextBox.Text = currentName;
        Dispatcher.UIThread.Post(() => NameTextBox.SelectAll());
    }

    private void OnCancel(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close(null);
    }

    private void OnOK(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Close(NameTextBox.Text);
    }
}