using System;

namespace PrimalEditor_Avalonia.Components;

public class MSCamera : MSComponentBase<CameraComponent>
{
    private CameraProjection? _projection;
    private float? _fov, _orthoSize, _near, _far;
    private float? _clearR, _clearG, _clearB, _clearA;

    public CameraProjection? Projection { get => _projection; set { _projection = value; OnPropertyChanged(); } }
    public float? Fov       { get => _fov;       set { if (!Same(_fov, value))       { _fov       = value; OnPropertyChanged(); } } }
    public float? OrthoSize { get => _orthoSize; set { if (!Same(_orthoSize, value)) { _orthoSize = value; OnPropertyChanged(); } } }
    public float? NearPlane { get => _near;      set { if (!Same(_near, value))      { _near      = value; OnPropertyChanged(); } } }
    public float? FarPlane  { get => _far;       set { if (!Same(_far, value))       { _far       = value; OnPropertyChanged(); } } }
    public float? ClearColorR { get => _clearR; set { if (!Same(_clearR, value)) { _clearR = value; OnPropertyChanged(); } } }
    public float? ClearColorG { get => _clearG; set { if (!Same(_clearG, value)) { _clearG = value; OnPropertyChanged(); } } }
    public float? ClearColorB { get => _clearB; set { if (!Same(_clearB, value)) { _clearB = value; OnPropertyChanged(); } } }
    public float? ClearColorA { get => _clearA; set { if (!Same(_clearA, value)) { _clearA = value; OnPropertyChanged(); } } }

    public MSCamera(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _projection = AggregateEnum(c => c.Projection);
        _fov       = AggregateFloat(c => c.FieldOfViewDegrees);
        _orthoSize = AggregateFloat(c => c.OrthographicSize);
        _near      = AggregateFloat(c => c.NearPlane);
        _far       = AggregateFloat(c => c.FarPlane);
        _clearR    = AggregateFloat(c => c.ClearColorR);
        _clearG    = AggregateFloat(c => c.ClearColorG);
        _clearB    = AggregateFloat(c => c.ClearColorB);
        _clearA    = AggregateFloat(c => c.ClearColorA);
        OnPropertyChanged(nameof(Projection));
        OnPropertyChanged(nameof(Fov)); OnPropertyChanged(nameof(OrthoSize));
        OnPropertyChanged(nameof(NearPlane)); OnPropertyChanged(nameof(FarPlane));
        OnPropertyChanged(nameof(ClearColorR)); OnPropertyChanged(nameof(ClearColorG));
        OnPropertyChanged(nameof(ClearColorB)); OnPropertyChanged(nameof(ClearColorA));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var cam in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(Projection):  if (_projection.HasValue) cam.Projection = _projection.Value; break;
                case nameof(Fov):         if (_fov.HasValue) cam.FieldOfViewDegrees = _fov.Value; break;
                case nameof(OrthoSize):   if (_orthoSize.HasValue) cam.OrthographicSize = _orthoSize.Value; break;
                case nameof(NearPlane):   if (_near.HasValue) cam.NearPlane = _near.Value; break;
                case nameof(FarPlane):    if (_far.HasValue) cam.FarPlane = _far.Value; break;
                case nameof(ClearColorR): if (_clearR.HasValue) cam.ClearColorR = _clearR.Value; break;
                case nameof(ClearColorG): if (_clearG.HasValue) cam.ClearColorG = _clearG.Value; break;
                case nameof(ClearColorB): if (_clearB.HasValue) cam.ClearColorB = _clearB.Value; break;
                case nameof(ClearColorA): if (_clearA.HasValue) cam.ClearColorA = _clearA.Value; break;
            }
        }
        return true;
    }

    private float? AggregateFloat(Func<CameraComponent, float> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var c in SelectedComponents)
            if (Math.Abs(sel(c) - v) > 1e-4f) return null;
        return v;
    }

    private CameraProjection? AggregateEnum(Func<CameraComponent, CameraProjection> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var c in SelectedComponents)
            if (sel(c) != v) return null;
        return v;
    }
}
