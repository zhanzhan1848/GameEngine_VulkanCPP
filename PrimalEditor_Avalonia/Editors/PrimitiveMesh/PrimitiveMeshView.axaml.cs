using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public partial class PrimitiveMeshView : UserControl
{
    public PrimitiveMeshView() { InitializeComponent(); }
    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}