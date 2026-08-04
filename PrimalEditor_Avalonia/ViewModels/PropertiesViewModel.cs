using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class PropertiesViewModel : ViewModelBase
{
    private MSEntity _msEntity = new();
    private string _mixedValueMessage = string.Empty;

    public bool HasSelection => _msEntity.SelectedEntities.Count > 0;
    public bool HasMultipleSelection => _msEntity.SelectedEntities.Count > 1;
    public bool HasMixedValues => !string.IsNullOrEmpty(_mixedValueMessage);

    public MSEntity MSEntity
    {
        get => _msEntity;
        private set
        {
            _msEntity = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(HasSelection));
            OnPropertyChanged(nameof(HasMultipleSelection));
        }
    }

    public string MixedValueMessage
    {
        get => _mixedValueMessage;
        set
        {
            if (_mixedValueMessage != value)
            {
                _mixedValueMessage = value;
                OnPropertyChanged(nameof(MixedValueMessage));
                OnPropertyChanged(nameof(HasMixedValues));
            }
        }
    }

    public MSTransform? Transform { get; private set; }
    public MSLight? Light { get; private set; }
    public MSCamera? Camera { get; private set; }
    public MSMeshRenderer? MeshRenderer { get; private set; }

    public System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel> MaterialOverrides { get; }
        = new System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel>();

    public GameEntity[] Entities
    {
        set
        {
            var ms = new MSEntity();
            if (value != null)
                foreach (var e in value) ms.SelectedEntities.Add(e);
            MSEntity = ms;

            if (ms.SelectedEntities.Count == 0)
            {
                Transform = null;
                Light = null;
                Camera = null;
                MeshRenderer = null;
                MaterialOverrides.Clear();
            }
            else
            {
                Transform = new MSTransform(ms);
                Light = new MSLight(ms);
                Camera = new MSCamera(ms);
                MeshRenderer = new MSMeshRenderer(ms);
                RebuildMaterialOverrides(ms.SelectedEntities);
            }
            UpdateFromEntities();
        }
    }

    private void RebuildMaterialOverrides(System.Collections.Generic.IList<Components.GameEntity> entities)
    {
        MaterialOverrides.Clear();
        if (entities.Count == 0) return;
        var first = entities[0];
        var firstOverride = first.GetComponent<Components.MaterialOverrideComponent>();
        if (firstOverride == null) return;
        for (int i = 0; i < firstOverride.Slots.Count; i++)
        {
            MaterialOverrides.Add(new MaterialOverrideViewModel(firstOverride.Slots[i], i));
        }
    }

    public string EntityName
    {
        get => _msEntity.Name;
        set
        {
            if (_msEntity.SelectedEntities.Count == 1 && _msEntity.SelectedEntities[0].Name != value)
            {
                _msEntity.SelectedEntities[0].Name = value;
                OnPropertyChanged();
            }
        }
    }

    public bool IsEnabled
    {
        get => _msEntity.IsEnabled;
        set { _msEntity.IsEnabled = value; OnPropertyChanged(); }
    }

    public float? PosX   { get => Transform?.PosX;   set { if (Transform != null) { RecordAndApply(nameof(PosX),   () => Transform.PosX   = value); } } }
    public float? PosY   { get => Transform?.PosY;   set { if (Transform != null) { RecordAndApply(nameof(PosY),   () => Transform.PosY   = value); } } }
    public float? PosZ   { get => Transform?.PosZ;   set { if (Transform != null) { RecordAndApply(nameof(PosZ),   () => Transform.PosZ   = value); } } }
    public float? RotX   { get => Transform?.RotX;   set { if (Transform != null) { RecordAndApply(nameof(RotX),   () => Transform.RotX   = value); } } }
    public float? RotY   { get => Transform?.RotY;   set { if (Transform != null) { RecordAndApply(nameof(RotY),   () => Transform.RotY   = value); } } }
    public float? RotZ   { get => Transform?.RotZ;   set { if (Transform != null) { RecordAndApply(nameof(RotZ),   () => Transform.RotZ   = value); } } }
    public float? ScaleX { get => Transform?.ScaleX; set { if (Transform != null) { RecordAndApply(nameof(ScaleX), () => Transform.ScaleX = value); } } }
    public float? ScaleY { get => Transform?.ScaleY; set { if (Transform != null) { RecordAndApply(nameof(ScaleY), () => Transform.ScaleY = value); } } }
    public float? ScaleZ { get => Transform?.ScaleZ; set { if (Transform != null) { RecordAndApply(nameof(ScaleZ), () => Transform.ScaleZ = value); } } }

    private void RecordAndApply(string propertyName, Action applyChange)
    {
        // Capture snapshot BEFORE applying the change so the old state is the
        // pre-change transform, and the new state is the post-change one.
        var undoRedo = TryGetUndoRedo();
        if (undoRedo == null || undoRedo.IsUndoingOrRedoing)
        {
            // Undo/Redo path: apply change, no record.
            applyChange();
            OnPropertyChanged(propertyName);
            return;
        }

        var oldDataList = SnapshotSelected();
        applyChange();
        var newDataList = SnapshotSelected();
        OnPropertyChanged(propertyName);

        if (oldDataList.Count == 0 || HasAnyChange(oldDataList, newDataList))
        {
            undoRedo.Add(new MultiEntityTransformAction(oldDataList, newDataList));
            MarkProjectDirty();
        }
    }

    private void MarkProjectDirty()
    {
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is MainWindowViewModel mainVm
            && mainVm.Project != null)
        {
            mainVm.Project.MarkDirty();
            mainVm.Project.ActiveScene?.MarkDirty();
        }
    }

    private List<(GameEntity entity, TransformData data)> SnapshotSelected()
    {
        var list = new List<(GameEntity entity, TransformData data)>();
        foreach (var e in _msEntity.SelectedEntities)
        {
            var t = e.GetTransform();
            if (t == null) continue;
            list.Add((e, TransformData.FromTransform(t)));
        }
        return list;
    }

    private static bool HasAnyChange(
        List<(GameEntity entity, TransformData data)> oldList,
        List<(GameEntity entity, TransformData data)> newList)
    {
        if (oldList.Count != newList.Count) return true;
        for (int i = 0; i < oldList.Count; ++i)
        {
            if (!TransformDataEquals(oldList[i].data, newList[i].data)) return true;
        }
        return false;
    }

    private static bool TransformDataEquals(in TransformData a, in TransformData b)
    {
        const float Eps = 1e-4f;
        return System.Math.Abs(a.PositionX - b.PositionX) < Eps
            && System.Math.Abs(a.PositionY - b.PositionY) < Eps
            && System.Math.Abs(a.PositionZ - b.PositionZ) < Eps
            && System.Math.Abs(a.RotationX - b.RotationX) < Eps
            && System.Math.Abs(a.RotationY - b.RotationY) < Eps
            && System.Math.Abs(a.RotationZ - b.RotationZ) < Eps
            && System.Math.Abs(a.ScaleX - b.ScaleX) < Eps
            && System.Math.Abs(a.ScaleY - b.ScaleY) < Eps
            && System.Math.Abs(a.ScaleZ - b.ScaleZ) < Eps;
    }

    private UndoRedo? TryGetUndoRedo()
    {
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is MainWindowViewModel mainVm)
        {
            return mainVm.WorldEditor.UndoRedo;
        }
        return null;
    }

    private void UpdateFromEntities()
    {
        OnPropertyChanged(nameof(EntityName));
        OnPropertyChanged(nameof(IsEnabled));
        OnPropertyChanged(nameof(PosX));   OnPropertyChanged(nameof(PosY));   OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX));   OnPropertyChanged(nameof(RotY));   OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX)); OnPropertyChanged(nameof(ScaleY)); OnPropertyChanged(nameof(ScaleZ));

        Light?.Refresh();
        Camera?.Refresh();
        MeshRenderer?.Refresh();
        OnPropertyChanged(nameof(Light));
        OnPropertyChanged(nameof(Camera));
        OnPropertyChanged(nameof(MeshRenderer));

        MixedValueMessage = _msEntity.SelectedEntities.Count > 1
            ? "Multiple entities selected - showing first value or '-' for mixed"
            : string.Empty;
    }
}