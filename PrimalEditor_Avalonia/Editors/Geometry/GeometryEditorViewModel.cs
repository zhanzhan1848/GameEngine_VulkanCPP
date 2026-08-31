using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public class GeometryEditorViewModel : AssetEditorViewModel, IDirtyTrackable
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
            OnPropertyChanged(nameof(SubmeshCount));
            OnPropertyChanged(nameof(BoundsSummary));
        }
    }

    public GeometryImportSettings Settings { get; private set; } = new();

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; OnPropertyChanged(); OnPropertyChanged(nameof(CanSave)); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;

    public bool CanSave => Asset != null && IsDirty;

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
                OnPropertyChanged(nameof(SubmeshCount));
                OnPropertyChanged(nameof(BoundsSummary));
            }
        }
    }

    public MeshLOD? SelectedLOD => _geometry.LODs.ElementAtOrDefault(_lodIndex);

    private bool _autoLOD = true;
    public bool AutoLOD
    {
        get => _autoLOD;
        set { if (_autoLOD != value) { _autoLOD = value; OnPropertyChanged(); MarkDirty(); } }
    }

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

    public int SubmeshCount => SelectedLOD?.Meshes.Count ?? 0;

    public string BoundsSummary
    {
        get
        {
            if (SelectedLOD == null || SelectedLOD.Meshes.Count == 0) return "(no bounds)";
            float minX = float.MaxValue, minY = float.MaxValue, minZ = float.MaxValue;
            float maxX = float.MinValue, maxY = float.MinValue, maxZ = float.MinValue;
            foreach (var m in SelectedLOD.Meshes)
            {
                var pos = m.GetPositionsAsFloat3();
                for (int i = 0; i < pos.Length; i += 3)
                {
                    float x = pos[i], y = pos[i + 1], z = pos[i + 2];
                    if (x < minX) minX = x; if (x > maxX) maxX = x;
                    if (y < minY) minY = y; if (y > maxY) maxY = y;
                    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
                }
            }
            return $"BBox: ({minX:0.##},{minY:0.##},{minZ:0.##}) → ({maxX:0.##},{maxY:0.##},{maxZ:0.##})";
        }
    }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        Settings = AssetImportSettingsStore.Load<GeometryImportSettings>(info.FullPath);
        ClearDirty();
        State = AssetEditorState.Loading;

        // When the user opens a model container (.model.json) we have to
        // load the cooked .mesh sibling, not the sidecar — Geometry.Load
        // is a binary reader for the cooked format. info.FullPath is the
        // sidecar in the new no-copy design (source .fbx no longer lives
        // inside the project).
        var meshPath = ResolveCookedMeshPath(info);
        if (meshPath == null)
        {
            Logger.Error($"Geometry load failed: cooked .mesh missing for {info.FullPath}");
            State = AssetEditorState.Done;
            OnPropertyChanged(nameof(CanSave));
            return;
        }

        var g = new Geometry();
        var ok = g.Load(meshPath);
        if (ok)
        {
            Data = g;
            LODIndex = 0;
            Logger.Info($"Geometry loaded: {info.FileName} — {_geometry.LODs.Count} LOD(s)");
        }
        else
        {
            Logger.Error($"Geometry load failed: {meshPath}");
        }
        State = AssetEditorState.Done;
        OnPropertyChanged(nameof(CanSave));
    }

    private static string? ResolveCookedMeshPath(AssetInfo info)
    {
        if (info.Type == AssetType.Mesh)
        {
            // Legacy case: .mesh file or pre-no-copy .fbx whose sibling
            // .mesh exists. info.FullPath is the user-visible file.
            return info.FullPath;
        }
        if (info.Type == AssetType.Model)
        {
            // New no-copy design: info.FullPath is the .model.json
            // sidecar. Sidecar.Mesh is the canonical pointer when
            // present; otherwise fall back to the sibling .mesh file
            // (Path.ChangeExtension is unsafe on .model.json because
            // Path.GetExtension only returns ".json").
            var sidecar = ModelSidecar.Load(info.FullPath);
            if (sidecar != null && !string.IsNullOrEmpty(sidecar.Mesh) && System.IO.File.Exists(sidecar.Mesh))
                return sidecar.Mesh;
            var stem = System.IO.Path.GetFileNameWithoutExtension(info.FullPath);
            var sibling = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(info.FullPath) ?? string.Empty, stem + ".mesh");
            return System.IO.File.Exists(sibling) ? sibling : null;
        }
        return info.FullPath;
    }

    public void Reimport()
    {
        if (Asset == null) return;
        State = AssetEditorState.Processing;
        OnPropertyChanged(nameof(CanSave));
        Logger.Info($"Reimport: {Asset.FullPath} — settings applied; mesh data unchanged (engine-side Reimport is a future hook)");
        Save();
        State = AssetEditorState.Done;
        OnPropertyChanged(nameof(CanSave));
    }

    public override void Save()
    {
        if (Asset == null) return;
        var ok = AssetImportSettingsStore.Save(Asset.FullPath, Settings);
        if (ok)
        {
            ClearDirty();
            Logger.Info($"Saved geometry settings: {Asset.FileName}");
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        Settings = AssetImportSettingsStore.Load<GeometryImportSettings>(Asset.FullPath);
        ClearDirty();
        OnPropertyChanged(nameof(AutoLOD));
        OnPropertyChanged(nameof(CanSave));
    }
}
