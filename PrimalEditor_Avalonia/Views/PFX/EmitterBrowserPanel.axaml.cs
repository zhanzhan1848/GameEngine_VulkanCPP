using Avalonia.Controls;
using Avalonia.Input;
using System;
using System.Linq;

namespace PrimalEditor_Avalonia.Views.PFX;

public partial class EmitterBrowserPanel : UserControl
{
    public event Action<string>? AssetOpenRequested;

    public EmitterBrowserPanel()
    {
        InitializeComponent();
        DataContext = new PrimalEditor_Avalonia.ViewModels.PFX.PfxBrowserPanelViewModel();
    }

    private void OnAssetDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (DataContext is PrimalEditor_Avalonia.ViewModels.PFX.PfxBrowserPanelViewModel vm &&
            vm.SelectedAsset != null)
        {
            AssetOpenRequested?.Invoke(vm.SelectedAsset.FullPath);
        }
    }
}
