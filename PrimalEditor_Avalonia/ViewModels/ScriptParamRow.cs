using System.ComponentModel;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Inspector Lua 参数编辑行（scripting S1 收官）。Name 只读（键是脚本契约，
/// 换键 = 删旧加新）；Value 为字符串原样编辑——引擎侧 params 表按 Lua 算术
/// 自然转型，这里不做类型校验。行是编辑缓冲，落盘/重建实例由
/// LuaApplyParamsCommand 一次性提交（与 script.set-param CLI 同语义）。
/// </summary>
public sealed class ScriptParamRow : INotifyPropertyChanged
{
    private string _name = "";
    private string _value = "";

    public ScriptParamRow(string name, string value)
    {
        _name = name;
        _value = value;
    }

    public string Name
    {
        get => _name;
        set { _name = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Name))); }
    }

    public string Value
    {
        get => _value;
        set { _value = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Value))); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}
