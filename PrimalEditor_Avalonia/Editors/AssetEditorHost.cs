using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Editors;

public class AssetEditorHost : Window
{
    public AssetEditorViewModel ViewModel { get; }

    public AssetEditorHost(AssetEditorViewModel vm)
    {
        ViewModel = vm;
        DataContext = vm;
        Title = $"{vm.Title} — PrimalEditor";
        Width = 900;
        Height = 700;
        Content = BuildContent(vm);
    }

    private static object BuildContent(AssetEditorViewModel vm)
    {
        return vm switch
        {
            Texture.TextureEditorViewModel => new Texture.TextureEditorView { DataContext = vm },
            Geometry.GeometryEditorViewModel => new Geometry.GeometryEditorView { DataContext = vm },
            PrimitiveMesh.PrimitiveMeshViewModel => new PrimitiveMesh.PrimitiveMeshView { DataContext = vm },
            _ => new TextBlock { Text = $"Editor for {vm.GetType().Name} not yet implemented" }
        };
    }
}