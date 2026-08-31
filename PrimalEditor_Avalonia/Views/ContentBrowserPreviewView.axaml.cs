using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class ContentBrowserPreviewView : UserControl
{
    public ContentBrowserPreviewView()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }
}