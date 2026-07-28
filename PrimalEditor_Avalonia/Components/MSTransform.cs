using System;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class MSTransform : MSComponent<Transform>
{
    private float? _posX, _posY, _posZ, _rotX, _rotY, _rotZ, _scaleX, _scaleY, _scaleZ;

    public float? PosX   { get => _posX;   set { if (!Same(_posX, value))   { _posX   = value; OnPropertyChanged(); } } }
    public float? PosY   { get => _posY;   set { if (!Same(_posY, value))   { _posY   = value; OnPropertyChanged(); } } }
    public float? PosZ   { get => _posZ;   set { if (!Same(_posZ, value))   { _posZ   = value; OnPropertyChanged(); } } }
    public float? RotX   { get => _rotX;   set { if (!Same(_rotX, value))   { _rotX   = value; OnPropertyChanged(); } } }
    public float? RotY   { get => _rotY;   set { if (!Same(_rotY, value))   { _rotY   = value; OnPropertyChanged(); } } }
    public float? RotZ   { get => _rotZ;   set { if (!Same(_rotZ, value))   { _rotZ   = value; OnPropertyChanged(); } } }
    public float? ScaleX { get => _scaleX; set { if (!Same(_scaleX, value)) { _scaleX = value; OnPropertyChanged(); } } }
    public float? ScaleY { get => _scaleY; set { if (!Same(_scaleY, value)) { _scaleY = value; OnPropertyChanged(); } } }
    public float? ScaleZ { get => _scaleZ; set { if (!Same(_scaleZ, value)) { _scaleZ = value; OnPropertyChanged(); } } }

    public MSTransform(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _posX   = Aggregate(t => t.PositionX);
        _posY   = Aggregate(t => t.PositionY);
        _posZ   = Aggregate(t => t.PositionZ);
        _rotX   = Aggregate(t => t.RotationX);
        _rotY   = Aggregate(t => t.RotationY);
        _rotZ   = Aggregate(t => t.RotationZ);
        _scaleX = Aggregate(t => t.ScaleX);
        _scaleY = Aggregate(t => t.ScaleY);
        _scaleZ = Aggregate(t => t.ScaleZ);
        OnPropertyChanged(nameof(PosX));   OnPropertyChanged(nameof(PosY));   OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX));   OnPropertyChanged(nameof(RotY));   OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX)); OnPropertyChanged(nameof(ScaleY)); OnPropertyChanged(nameof(ScaleZ));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var t in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(PosX):   if (_posX  .HasValue) t.PositionX = _posX.Value;   break;
                case nameof(PosY):   if (_posY  .HasValue) t.PositionY = _posY.Value;   break;
                case nameof(PosZ):   if (_posZ  .HasValue) t.PositionZ = _posZ.Value;   break;
                case nameof(RotX):   if (_rotX  .HasValue) t.RotationX = _rotX.Value;   break;
                case nameof(RotY):   if (_rotY  .HasValue) t.RotationY = _rotY.Value;   break;
                case nameof(RotZ):   if (_rotZ  .HasValue) t.RotationZ = _rotZ.Value;   break;
                case nameof(ScaleX): if (_scaleX.HasValue) t.ScaleX    = _scaleX.Value; break;
                case nameof(ScaleY): if (_scaleY.HasValue) t.ScaleY    = _scaleY.Value; break;
                case nameof(ScaleZ): if (_scaleZ.HasValue) t.ScaleZ    = _scaleZ.Value; break;
            }
        }
        return true;
    }

    private float? Aggregate(Func<Transform, float> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var t in SelectedComponents)
            if (Math.Abs(sel(t) - v) > 1e-4f) return null;
        return v;
    }

    private static bool Same(float? a, float? b)
    {
        if (a == null && b == null) return true;
        if (a == null || b == null) return false;
        return Math.Abs(a.Value - b.Value) < 1e-4f;
    }
}