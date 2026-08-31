using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors;

public partial class AssetEditorStatusBar : UserControl
{
    private TextBlock? _assetPathText;
    private TextBlock? _stateText;
    private TextBlock? _dirtyText;
    private AssetEditorViewModel? _wired;

    public AssetEditorStatusBar()
    {
        InitializeComponent();
        _assetPathText = this.FindControl<TextBlock>("AssetPathText");
        _stateText = this.FindControl<TextBlock>("StateText");
        _dirtyText = this.FindControl<TextBlock>("DirtyText");
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    public static readonly StyledProperty<AssetEditorViewModel?> ViewModelProperty =
        AvaloniaProperty.Register<AssetEditorStatusBar, AssetEditorViewModel?>(nameof(ViewModel));

    public AssetEditorViewModel? ViewModel
    {
        get => GetValue(ViewModelProperty);
        set { SetValue(ViewModelProperty, value); BindDataContext(value); }
    }

    private void BindDataContext(AssetEditorViewModel? vm)
    {
        if (_wired != null) _wired.PropertyChanged -= OnVmPropertyChanged;
        _wired = vm;
        if (vm != null) vm.PropertyChanged += OnVmPropertyChanged;
        OnVmPropertyChanged(this, new PropertyChangedEventArgs(null));
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_wired == null) return;
        if (e.PropertyName == nameof(AssetEditorViewModel.State)
            || e.PropertyName == nameof(AssetEditorViewModel.Asset)
            || e.PropertyName == null)
        {
            if (_stateText != null) _stateText.Text = _wired.State.ToString();
            if (_assetPathText != null) _assetPathText.Text = _wired.Asset?.FullPath ?? "(no asset)";
        }
        if (e.PropertyName == nameof(AssetEditorViewModel.IsDirty)
            || e.PropertyName == null)
        {
            if (_dirtyText != null) _dirtyText.Text = _wired.IsDirty ? "*" : "";
        }
    }
}
