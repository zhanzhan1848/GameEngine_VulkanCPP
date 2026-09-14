using System.Collections.ObjectModel;
using System.ComponentModel;

namespace PrimalEditor_Avalonia.ViewModels.PFX;

/// <summary>单个模块的 ViewModel（编辑器三列布局的行）。</summary>
public sealed class PfxModuleViewModel : INotifyPropertyChanged
{
    private string _name = "";
    private string _stage = "";
    private bool _enabled = true;
    private string _paramsJson = "{}";

    public string Name { get => _name; set { _name = value; PropertyChanged?.Invoke(this, new(nameof(Name))); } }
    public string Stage { get => _stage; set { _stage = value; PropertyChanged?.Invoke(this, new(nameof(Stage))); } }
    public bool Enabled { get => _enabled; set { _enabled = value; PropertyChanged?.Invoke(this, new(nameof(Enabled))); } }
    public string ParamsJson { get => _paramsJson; set { _paramsJson = value; PropertyChanged?.Invoke(this, new(nameof(ParamsJson))); } }

    /// <summary>该模块的参数编辑 JSON 文本（v1 手编，v2 schema 驱动动态控件）。</summary>
    public string DisplayText => $"{Name}  [{Stage}]";

    public event PropertyChangedEventHandler? PropertyChanged;
}

/// <summary>阶段分组行。</summary>
public sealed class PfxStageGroup
{
    public string StageName { get; set; } = "";
    public ObservableCollection<PfxModuleViewModel> Modules { get; } = new();
}
