using System;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public class GeometryEditorViewModel : AssetEditorViewModel
{
    private Geometry _geometry = new();
    public Geometry Data
    {
        get => _geometry;
        private set
        {
            _geometry = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(MaxLODIndex));
            OnPropertyChanged(nameof(SelectedLOD));
            OnPropertyChanged(nameof(LODList));
        }
    }

    public int MaxLODIndex => Math.Max(0, _geometry.LODs.Count - 1);

    public System.Collections.Generic.IReadOnlyList<int> LODList
    {
        get
        {
            var list = new System.Collections.Generic.List<int>();
            for (var i = 0; i <= MaxLODIndex; i++) list.Add(i);
            return list;
        }
    }

    private int _lodIndex;
    public int LODIndex
    {
        get => _lodIndex;
        set
        {
            var clamped = Math.Clamp(value, 0, MaxLODIndex);
            if (_lodIndex != clamped)
            {
                _lodIndex = clamped;
                OnPropertyChanged();
                OnPropertyChanged(nameof(SelectedLOD));
                OnPropertyChanged(nameof(VertexCount));
                OnPropertyChanged(nameof(TriangleCount));
            }
        }
    }

    public MeshLOD? SelectedLOD => _geometry.LODs.ElementAtOrDefault(_lodIndex);

    private bool _autoLOD = true;
    public bool AutoLOD { get => _autoLOD; set { _autoLOD = value; OnPropertyChanged(); } }

    public int VertexCount
    {
        get
        {
            if (SelectedLOD == null) return 0;
            return SelectedLOD.Meshes.Sum(m => m.VertexCount);
        }
    }

    public int TriangleCount
    {
        get
        {
            if (SelectedLOD == null) return 0;
            return SelectedLOD.Meshes.Sum(m => m.IndexCount / 3);
        }
    }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        State = AssetEditorState.Loading;
        var g = new Geometry();
        var ok = g.Load(info.FullPath);
        if (ok)
        {
            Data = g;
            LODIndex = 0;
            Logger.Info($"Geometry loaded: {info.FileName} — {_geometry.LODs.Count} LOD(s)");
        }
        else
        {
            Logger.Error($"Geometry load failed: {info.FullPath}");
        }
        State = AssetEditorState.Done;
    }
}