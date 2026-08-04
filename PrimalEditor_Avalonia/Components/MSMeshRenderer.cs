using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Components;

public class MSMeshRenderer : MSComponentBase<MeshRendererComponent>
{
    private AssetGuid? _meshGuid;
    private bool? _castShadow, _receiveShadow;

    public AssetGuid? MeshGuid { get => _meshGuid; set { _meshGuid = value; OnPropertyChanged(); } }
    public bool? CastShadow { get => _castShadow; set { _castShadow = value; OnPropertyChanged(); } }
    public bool? ReceiveShadow { get => _receiveShadow; set { _receiveShadow = value; OnPropertyChanged(); } }

    /// <summary>
    /// Material slot list merged across the selection. Each entry has
    /// a material asset Guid; empty Guids mean "use the parent
    /// MeshRenderer's material". Toggling <see cref="AddMaterial"/> /
    /// <see cref="RemoveMaterialAt"/> pushes changes back to every
    /// selected mesh renderer.
    /// </summary>
    public IList<Guid> MaterialGuidList
    {
        get => _materialGuidList;
        private set { _materialGuidList = value; OnPropertyChanged(); }
    }
    private IList<Guid> _materialGuidList = Array.Empty<Guid>();

    public MSMeshRenderer(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _meshGuid       = AggregateGuid(m => m.MeshGuid);
        _castShadow     = AggregateBool(m => m.CastShadow);
        _receiveShadow  = AggregateBool(m => m.ReceiveShadow);
        OnPropertyChanged(nameof(MeshGuid));
        OnPropertyChanged(nameof(CastShadow));
        OnPropertyChanged(nameof(ReceiveShadow));
        MaterialGuidList = AggregateMaterialList();
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var renderer in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(MeshGuid):      if (_meshGuid.HasValue) renderer.MeshGuid = _meshGuid.Value; break;
                case nameof(CastShadow):    if (_castShadow.HasValue) renderer.CastShadow = _castShadow.Value; break;
                case nameof(ReceiveShadow): if (_receiveShadow.HasValue) renderer.ReceiveShadow = _receiveShadow.Value; break;
                case nameof(MaterialGuidList): ApplyMaterialList(renderer); break;
            }
        }
        return true;
    }

    private AssetGuid? AggregateGuid(Func<MeshRendererComponent, AssetGuid> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        if (v.Value == Guid.Empty) return null;
        foreach (var r in SelectedComponents)
            if (sel(r).Value != v.Value) return null;
        return v;
    }

    private bool? AggregateBool(Func<MeshRendererComponent, bool> sel)
    {
        if (SelectedComponents.Count == 0) return null;
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var r in SelectedComponents)
            if (sel(r) != v) return null;
        return v;
    }

    private List<Guid> AggregateMaterialList()
    {
        if (SelectedComponents.Count == 0) return new List<Guid>();
        // Use the first renderer's list as the canonical ordering;
        // ignore other renderers if they don't match — the inspector
        // will show the first one and the user can add/remove to
        // bring the rest in line.
        return SelectedComponents[0].MaterialGuids.Select(g => g.Value).ToList();
    }

    private void ApplyMaterialList(MeshRendererComponent renderer)
    {
        renderer.MaterialGuids.Clear();
        foreach (var guid in _materialGuidList)
            renderer.MaterialGuids.Add(new AssetGuid(guid));
    }
}
