using System;

namespace PrimalEditor_Avalonia.Components;

public class MSLight : MSComponentBase<LightComponent>
{
    private LightType? _lightType;
    private float? _intensity, _range, _umbra, _penumbra;
    private float? _colorR, _colorG, _colorB;
    private bool? _castShadow;

    public LightType? LightType { get => _lightType; set { _lightType = value; OnPropertyChanged(); } }
    public float? Intensity { get => _intensity; set { if (!Same(_intensity, value)) { _intensity = value; OnPropertyChanged(); } } }
    public float? Range { get => _range; set { if (!Same(_range, value)) { _range = value; OnPropertyChanged(); } } }
    public float? UmbraAngle { get => _umbra; set { if (!Same(_umbra, value)) { _umbra = value; OnPropertyChanged(); } } }
    public float? PenumbraAngle { get => _penumbra; set { if (!Same(_penumbra, value)) { _penumbra = value; OnPropertyChanged(); } } }
    public float? ColorR { get => _colorR; set { if (!Same(_colorR, value)) { _colorR = value; OnPropertyChanged(); } } }
    public float? ColorG { get => _colorG; set { if (!Same(_colorG, value)) { _colorG = value; OnPropertyChanged(); } } }
    public float? ColorB { get => _colorB; set { if (!Same(_colorB, value)) { _colorB = value; OnPropertyChanged(); } } }
    public bool? CastShadow { get => _castShadow; set { _castShadow = value; OnPropertyChanged(); } }

    public MSLight(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _lightType  = AggregateEnum(l => l.LightType);
        _intensity  = AggregateFloat(l => l.Intensity);
        _range      = AggregateFloat(l => l.Range);
        _umbra      = AggregateFloat(l => l.UmbraAngle);
        _penumbra   = AggregateFloat(l => l.PenumbraAngle);
        _colorR     = AggregateFloat(l => l.ColorR);
        _colorG     = AggregateFloat(l => l.ColorG);
        _colorB     = AggregateFloat(l => l.ColorB);
        _castShadow = AggregateBool(l => l.CastShadow);
        OnPropertyChanged(nameof(LightType));
        OnPropertyChanged(nameof(Intensity)); OnPropertyChanged(nameof(Range));
        OnPropertyChanged(nameof(UmbraAngle)); OnPropertyChanged(nameof(PenumbraAngle));
        OnPropertyChanged(nameof(ColorR)); OnPropertyChanged(nameof(ColorG)); OnPropertyChanged(nameof(ColorB));
        OnPropertyChanged(nameof(CastShadow));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var light in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(LightType):    if (_lightType.HasValue) light.LightType = _lightType.Value; break;
                case nameof(Intensity):    if (_intensity.HasValue) light.Intensity = _intensity.Value; break;
                case nameof(Range):        if (_range.HasValue) light.Range = _range.Value; break;
                case nameof(UmbraAngle):   if (_umbra.HasValue) light.UmbraAngle = _umbra.Value; break;
                case nameof(PenumbraAngle):if (_penumbra.HasValue) light.PenumbraAngle = _penumbra.Value; break;
                case nameof(ColorR):       if (_colorR.HasValue) light.ColorR = _colorR.Value; break;
                case nameof(ColorG):       if (_colorG.HasValue) light.ColorG = _colorG.Value; break;
                case nameof(ColorB):       if (_colorB.HasValue) light.ColorB = _colorB.Value; break;
                case nameof(CastShadow):   if (_castShadow.HasValue) light.CastShadow = _castShadow.Value; break;
            }
        }
        return true;
    }

    private float? AggregateFloat(Func<LightComponent, float> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var l in SelectedComponents)
            if (Math.Abs(sel(l) - v) > 1e-4f) return null;
        return v;
    }

    private bool? AggregateBool(Func<LightComponent, bool> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var l in SelectedComponents)
            if (sel(l) != v) return null;
        return v;
    }

    private LightType? AggregateEnum(Func<LightComponent, LightType> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var l in SelectedComponents)
            if (sel(l) != v) return null;
        return v;
    }
}
