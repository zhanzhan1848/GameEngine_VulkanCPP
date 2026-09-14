using Avalonia.Controls;

namespace PrimalEditor_Avalonia.Views.PFX;

public partial class EmitterEditorWindow : Window
{
    public EmitterEditorWindow()
    {
        InitializeComponent();
    }

    public EmitterEditorWindow(PrimalEditor_Avalonia.ViewModels.PFX.PfxEditorWindowViewModel vm) : this()
    {
        DataContext = vm;
    }
}
