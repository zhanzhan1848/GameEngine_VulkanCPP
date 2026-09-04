using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using PrimalEditor_Avalonia.Editors.Pcg;

namespace PrimalEditor_Avalonia.Editors.Pcg;

public partial class PcgEditorView : UserControl
{
    public PcgEditorView()
    {
        InitializeComponent();
    }

    private PcgEditorViewModel? Vm => DataContext as PcgEditorViewModel;

    private void OnPaletteAddClick(object? sender, RoutedEventArgs e)
    {
        if (Vm is null || sender is not Button { Content: string typeName }) return;
        Vm.AddNode(typeName);
    }

    private void OnNodePointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (Vm is null) return;
        if (e.GetCurrentPoint(this).Properties.IsRightButtonPressed &&
            sender is Control { DataContext: PcgNodeViewModel node })
        {
            Vm.DeleteNode(node);
            e.Handled = true;
        }
        else if (sender is Control { DataContext: PcgNodeViewModel selected })
        {
            Vm.SelectedNode = selected;
        }
    }
}
