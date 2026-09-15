using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace PrimalEditor_Avalonia.Editors.Particle;

/// <summary>
/// One editable parameter row of the particle preset editor's right panel.
/// A row is a scalar (float/int/bool/text/enum) or a float vector with 2-4
/// components rendered as X/Y/Z/W TextBoxes like the material editor's
/// parameter panel. Accessors read/write the underlying
/// <see cref="ParticlePresetDocument"/> directly; every mutation reports
/// through <paramref name="onChanged"/> so the editor marks itself dirty.
/// </summary>
public sealed class ParticleParamViewModel : INotifyPropertyChanged
{
    public enum ParamKind { Float, Int, Bool, Text, Enum, Vector }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    private readonly Action? _onChanged;
    private readonly Func<float> _getFloat;
    private readonly Action<float>? _setFloat;
    private readonly Func<int> _getInt;
    private readonly Action<int>? _setInt;
    private readonly Func<bool> _getBool;
    private readonly Action<bool>? _setBool;
    private readonly Func<string> _getText;
    private readonly Action<string>? _setText;
    private readonly Func<float[]> _getVec;
    private readonly Action<float[]>? _setVec;

    public string Name { get; }
    public ParamKind Kind { get; }

    public bool IsFloat => Kind == ParamKind.Float;
    public bool IsInt => Kind == ParamKind.Int;
    public bool IsBool => Kind == ParamKind.Bool;
    public bool IsText => Kind == ParamKind.Text;
    public bool IsEnum => Kind == ParamKind.Enum;
    public bool IsVector => Kind == ParamKind.Vector;

    public ParticleParamViewModel(
        string name, ParamKind kind,
        Func<float>? getFloat = null, Action<float>? setFloat = null,
        Func<int>? getInt = null, Action<int>? setInt = null,
        Func<bool>? getBool = null, Action<bool>? setBool = null,
        Func<string>? getText = null, Action<string>? setText = null,
        Func<float[]>? getVec = null, Action<float[]>? setVec = null,
        Action? onChanged = null)
    {
        Name = name;
        Kind = kind;
        _getFloat = getFloat ?? (() => 0f);
        _setFloat = setFloat;
        _getInt = getInt ?? (() => 0);
        _setInt = setInt;
        _getBool = getBool ?? (() => false);
        _setBool = setBool;
        _getText = getText ?? (() => "");
        _setText = setText;
        _getVec = getVec ?? (() => Array.Empty<float>());
        _setVec = setVec;
        _onChanged = onChanged;
    }

    // Scalars ------------------------------------------------------------------

    public float FloatValue
    {
        get => _getFloat();
        set
        {
            if (_setFloat == null || Math.Abs(_getFloat() - value) < 1e-6f) return;
            _setFloat(value);
            _onChanged?.Invoke();
        }
    }

    public int IntValue
    {
        get => _getInt();
        set { if (_setInt == null) return; _setInt(value); _onChanged?.Invoke(); }
    }

    public bool BoolValue
    {
        get => _getBool();
        set { if (_setBool == null) return; _setBool(value); _onChanged?.Invoke(); }
    }

    public string TextValue
    {
        get => _getText();
        set { if (_setText == null) return; _setText(value); _onChanged?.Invoke(); }
    }

    public int EnumIndex
    {
        get => Math.Max(0, _getInt());
        set { if (_setInt == null) return; _setInt(value); _onChanged?.Invoke(); }
    }

    public string[] EnumOptions { get; init; } = Array.Empty<string>();

    // Vector components ----------------------------------------------------------

    public int ComponentCount { get; init; } = 3;
    public bool ShowX => IsVector && ComponentCount > 0;
    public bool ShowY => IsVector && ComponentCount > 1;
    public bool ShowZ => IsVector && ComponentCount > 2;
    public bool ShowW => IsVector && ComponentCount > 3;

    private float[] Snapshot() => _getVec() ?? new float[ComponentCount];

    public float X { get => Comp(0); set => SetComp(0, value); }
    public float Y { get => Comp(1); set => SetComp(1, value); }
    public float Z { get => Comp(2); set => SetComp(2, value); }
    public float W { get => Comp(3); set => SetComp(3, value); }

    private float Comp(int i)
    {
        var v = Snapshot();
        return i < v.Length ? v[i] : 0f;
    }

    private void SetComp(int i, float value)
    {
        if (_setVec == null) return;
        var v = Snapshot();
        if (i >= v.Length) return;
        if (Math.Abs(v[i] - value) < 1e-6f) return;
        v[i] = value;
        _setVec(v);
        _onChanged?.Invoke();
    }
}
