using System;

namespace PrimalEditor_Avalonia.Components;

/// <summary>Collider 组件的多选聚合 VM（最小实现）。</summary>
public class MSCollider : MSComponentBase<ColliderComponent>
{
    private int? _shape;
    private float? _halfX, _halfY, _halfZ;
    private bool? _isTrigger;
    private int? _layer;

    public int? Shape { get => _shape; set { _shape = value; OnPropertyChanged(); } }
    public float? HalfX { get => _halfX; set { if (!Same(_halfX, value)) { _halfX = value; OnPropertyChanged(); } } }
    public float? HalfY { get => _halfY; set { if (!Same(_halfY, value)) { _halfY = value; OnPropertyChanged(); } } }
    public float? HalfZ { get => _halfZ; set { if (!Same(_halfZ, value)) { _halfZ = value; OnPropertyChanged(); } } }
    public bool? IsTrigger { get => _isTrigger; set { _isTrigger = value; OnPropertyChanged(); } }
    public int? Layer { get => _layer; set { _layer = value; OnPropertyChanged(); } }

    public MSCollider(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _shape = AggregateInt(c => (int)c.Shape);
        _halfX = AggregateFloat(c => c.HalfX);
        _halfY = AggregateFloat(c => c.HalfY);
        _halfZ = AggregateFloat(c => c.HalfZ);
        _isTrigger = AggregateBool(c => c.IsTrigger);
        _layer = AggregateInt(c => c.Layer);
        OnPropertyChanged(nameof(Shape)); OnPropertyChanged(nameof(HalfX));
        OnPropertyChanged(nameof(HalfY)); OnPropertyChanged(nameof(HalfZ));
        OnPropertyChanged(nameof(IsTrigger)); OnPropertyChanged(nameof(Layer));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var c in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(Shape): if (_shape.HasValue) c.Shape = (ColliderShape)_shape.Value; break;
                case nameof(HalfX): if (_halfX.HasValue) c.HalfX = _halfX.Value; break;
                case nameof(HalfY): if (_halfY.HasValue) c.HalfY = _halfY.Value; break;
                case nameof(HalfZ): if (_halfZ.HasValue) c.HalfZ = _halfZ.Value; break;
                case nameof(IsTrigger): if (_isTrigger.HasValue) c.IsTrigger = _isTrigger.Value; break;
                case nameof(Layer): if (_layer.HasValue) c.Layer = _layer.Value; break;
            }
        }
        return true;
    }

    private bool? AggregateBool(Func<ColliderComponent, bool> sel)
    {
        bool? v = null;
        foreach (var c in SelectedComponents) { var b = sel(c); v = v == null ? b : (v.Value == b ? v : null); }
        return v;
    }

    private float? AggregateFloat(Func<ColliderComponent, float> sel)
    {
        float? v = null;
        foreach (var c in SelectedComponents) { var f = sel(c); v = v == null ? f : (Same(v, f) ? v : null); }
        return v;
    }

    private int? AggregateInt(Func<ColliderComponent, int> sel)
    {
        int? v = null;
        foreach (var c in SelectedComponents) { var e = sel(c); v = v == null ? e : (v.Value == e ? v : null); }
        return v;
    }
}

/// <summary>RigidBody 组件的多选聚合 VM（最小实现）。</summary>
public class MSRigidBody : MSComponentBase<RigidBodyComponent>
{
    private int? _mode;
    private float? _mass, _restitution, _friction;

    public int? Mode { get => _mode; set { _mode = value; OnPropertyChanged(); } }
    public float? Mass { get => _mass; set { if (!Same(_mass, value)) { _mass = value; OnPropertyChanged(); } } }
    public float? Restitution { get => _restitution; set { if (!Same(_restitution, value)) { _restitution = value; OnPropertyChanged(); } } }
    public float? Friction { get => _friction; set { if (!Same(_friction, value)) { _friction = value; OnPropertyChanged(); } } }

    public MSRigidBody(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _mode = AggregateInt(c => (int)c.Mode);
        _mass = AggregateFloat(c => c.Mass);
        _restitution = AggregateFloat(c => c.Restitution);
        _friction = AggregateFloat(c => c.Friction);
        OnPropertyChanged(nameof(Mode)); OnPropertyChanged(nameof(Mass));
        OnPropertyChanged(nameof(Restitution)); OnPropertyChanged(nameof(Friction));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var c in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(Mode): if (_mode.HasValue) c.Mode = (RigidBodyMode)_mode.Value; break;
                case nameof(Mass): if (_mass.HasValue) c.Mass = _mass.Value; break;
                case nameof(Restitution): if (_restitution.HasValue) c.Restitution = _restitution.Value; break;
                case nameof(Friction): if (_friction.HasValue) c.Friction = _friction.Value; break;
            }
        }
        return true;
    }

    private int? AggregateInt(Func<RigidBodyComponent, int> sel)
    {
        int? v = null;
        foreach (var c in SelectedComponents) { var e = sel(c); v = v == null ? e : (v.Value == e ? v : null); }
        return v;
    }

    private float? AggregateFloat(Func<RigidBodyComponent, float> sel)
    {
        float? v = null;
        foreach (var c in SelectedComponents) { var f = sel(c); v = v == null ? f : (Same(v, f) ? v : null); }
        return v;
    }
}
