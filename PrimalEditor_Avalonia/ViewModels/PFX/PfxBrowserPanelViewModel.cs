using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Windows.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Models.PFX;
using PrimalEditor_Avalonia.Services;

namespace PrimalEditor_Avalonia.ViewModels.PFX;

/// <summary>发射器浏览器面板 VM：扫描/过滤/新建/删除/双击打开。</summary>
public sealed class PfxBrowserPanelViewModel : INotifyPropertyChanged
{
    private string _assetDirectory = ".";
    private string _filterText = "";
    private PfxAssetEntry? _selected;

    public ObservableCollection<PfxAssetEntry> Assets { get; } = new();
    public ObservableCollection<PfxAssetEntry> FilteredAssets { get; } = new();

    public string AssetDirectory
    {
        get => _assetDirectory;
        set { _assetDirectory = value; Scan(); }
    }

    public string FilterText
    {
        get => _filterText;
        set { _filterText = value; ApplyFilter(); }
    }

    public PfxAssetEntry? SelectedAsset
    {
        get => _selected;
        set { _selected = value; PropertyChanged?.Invoke(this, new(nameof(SelectedAsset))); }
    }

    /// <summary>双击资产 → 请求打开编辑器。</summary>
    public event Action<string>? AssetOpenRequested;

    public ICommand NewAssetCommand { get; }
    public ICommand DeleteCommand { get; }

    public PfxBrowserPanelViewModel()
    {
        NewAssetCommand = new RelayCommand(_ => NewAsset());
        DeleteCommand = new RelayCommand(_ => DeleteSelected(), _ => _selected != null);
        Scan();
    }

    public void Scan()
    {
        Assets.Clear();
        var dir = _assetDirectory;
        if (!System.IO.Directory.Exists(dir)) return;
        try
        {
            foreach (var f in System.IO.Directory.GetFiles(dir, "*.pfx_emitter"))
                Assets.Add(new PfxAssetEntry { FullPath = System.IO.Path.GetFullPath(f) });
        }
        catch { }
        ApplyFilter();
    }

    private void ApplyFilter()
    {
        FilteredAssets.Clear();
        foreach (var a in Assets.OrderBy(a => a.DisplayName))
        {
            if (string.IsNullOrEmpty(_filterText) || a.DisplayName.Contains(_filterText, StringComparison.OrdinalIgnoreCase))
                FilteredAssets.Add(a);
        }
    }

    private void NewAsset()
    {
        // v1：生成一个最小模板 JSON → 让用户在编辑器窗口里填模块
        var name = $"new_emitter_{Assets.Count + 1}";
        var path = System.IO.Path.Combine(_assetDirectory, name + ".pfx_emitter");
        const string template = """{"name":"new_emitter","capacity":1000,"stages":{}}""";
        try { System.IO.File.WriteAllText(path, template); } catch { return; }
        Scan();
    }

    private void DeleteSelected()
    {
        if (_selected == null) return;
        try { System.IO.File.Delete(_selected.FullPath); } catch { }
        Scan();
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}
