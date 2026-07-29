using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Editors.Texture;

public partial class TextureEditorView : UserControl
{
    public TextureEditorView() { InitializeComponent(); }
    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnReimportClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is TextureEditorViewModel vm) vm.Reimport();
    }

    private void OnSaveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is TextureEditorViewModel vm) vm.Save();
    }

    private void OnDiscardClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is TextureEditorViewModel vm) vm.Discard();
    }
}
