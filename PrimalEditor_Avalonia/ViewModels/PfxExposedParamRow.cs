using System.ComponentModel;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>PFX 暴露参数行（DataGrid 可编辑行）。</summary>
public sealed class PfxExposedParamRow : INotifyPropertyChanged
{
    private string _name = "";
    private float _value;

    public string Name
    {
        get => _name;
        set { _name = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Name))); }
    }

    public float Value
    {
        get => _value;
        set { _value = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Value))); }
    }

    public string ValueStr
    {
        get => _value.ToString("F4");
        set { if (float.TryParse(value, out var f)) _value = f; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ValueStr))); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}
