using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Native;
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

    /// <summary>True when the selected entity has a CameraComponent — the
    /// "Look Through This Camera" button is visible iff true.</summary>
    public bool HasCameraComponent => Camera != null;

    /// <summary>"Look Through This Camera" or "Restore Editor Camera"
    /// depending on whether the viewport is currently rendering through
    /// this entity. Bound to the Properties panel button.</summary>
    public string LookThroughButtonLabel { get; private set; } = "Look Through This Camera";
    public string MainCameraButtonLabel { get; private set; } = "Set as Main Camera";

    /// <summary>Native entity id of the currently selected entity, or 0
    /// when nothing is selected. Set by <see cref="RefreshEntityContext"/>
    /// when the panel binds to a new selection.</summary>
    public uint SelectedEntityNativeId { get; private set; }

    public System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel> MaterialOverrides { get; } = new System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel>();

    /// <summary>Called by the view after constructing this VM so it can
    /// route LookThroughCamera requests back to the viewport. Set via
    /// dependency injection (the WorldEditorView passes a delegate that
    /// forwards to the EngineViewport3D).</summary>
    public System.Action<uint>? LookThroughCameraCallback { get; set; }

    /// <summary>Called by the view after constructing this VM. Forwards to
    /// the viewport so the panel can ask "are we currently looking
    /// through the selected entity?".</summary>
    public System.Func<uint, bool>? IsLookingThroughCameraQuery { get; set; }
    public System.Action? SetMainCameraCallback { get; set; }
    public System.Func<bool>? IsMainCameraQuery { get; set; }

    // Injected by the owning WorldEditorViewModel. Inspector write-back must
    // not depend on finding a particular TopLevel/DataContext while docking
    // or window teardown is changing the visual tree.
    public System.Action<System.Collections.Generic.IEnumerable<GameEntity>>? PushTransformsToNativeCallback { get; set; }
    public System.Action? MarkProjectDirtyCallback { get; set; }
    public System.Func<UndoRedo?>? UndoRedoQuery { get; set; }

    public void RefreshEntityContext(uint selectedNativeId)
    {
        SelectedEntityNativeId = selectedNativeId;
        UpdateLookThroughLabel();
        UpdateMainCameraLabel();
    }

    private void UpdateLookThroughLabel()
    {
        if (SelectedEntityNativeId == 0 || IsLookingThroughCameraQuery == null)
        {
            LookThroughButtonLabel = "Look Through This Camera";
            OnPropertyChanged(nameof(LookThroughButtonLabel));
            return;
        }
        LookThroughButtonLabel = IsLookingThroughCameraQuery(SelectedEntityNativeId)
            ? "Restore Editor Camera"
            : "Look Through This Camera";
        OnPropertyChanged(nameof(LookThroughButtonLabel));
    }

    /// <summary>Called from the view's button click handler. Toggles the
    /// viewport's "look through this entity" state. If already looking
    /// through this entity, switches back to the editor camera.</summary>
    public void OnLookThroughCameraClicked()
    {
        if (SelectedEntityNativeId == 0 || LookThroughCameraCallback == null) return;
        if (IsLookingThroughCameraQuery != null
            && IsLookingThroughCameraQuery(SelectedEntityNativeId))
        {
            // Already looking through this one → switch back to editor.
            LookThroughCameraCallback(0u);
        }
        else
        {
            LookThroughCameraCallback(SelectedEntityNativeId);
        }
        UpdateLookThroughLabel();
    }

    /// <summary>Called from the view whenever the viewport's look-through
    /// state changes externally (e.g. another panel or shortcut). Re-runs
    /// the label refresh so the button text matches reality.</summary>
    public void NotifyLookThroughChanged()
    {
        UpdateLookThroughLabel();
    }

    public void OnSetMainCameraClicked()
    {
        if (SelectedEntityNativeId == 0) return;
        SetMainCameraCallback?.Invoke();
        UpdateMainCameraLabel();
    }

    public void NotifyMainCameraChanged() => UpdateMainCameraLabel();

    private void UpdateMainCameraLabel()
    {
        MainCameraButtonLabel = IsMainCameraQuery?.Invoke() == true
            ? "Main Camera"
            : "Set as Main Camera";
        OnPropertyChanged(nameof(MainCameraButtonLabel));
    }

    /// <summary>Stable list the Light.Type ComboBox binds against. Exposed
    /// here (not on MSLight) so the binding source survives across
    /// selection changes — MSLight is rebuilt every time the user picks
/// <summary>Stable list the Light.Type ComboBox binds against. Exposed
    /// here (not on MSLight) so the binding source survives across
    /// selection changes — MSLight is rebuilt every time the user picks
    /// a new entity, which would invalidate any ItemsSource bound to a
    /// view-model property on it.</summary>
    public IReadOnlyList<LightType> LightTypeOptions { get; } = new[] { LightType.Directional, LightType.Point, LightType.Spot };

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
        var oldDataList = SnapshotSelected();
        applyChange();
        var newDataList = SnapshotSelected();
        OnPropertyChanged(propertyName);

        var changed = oldDataList.Count == 0 || HasAnyChange(oldDataList, newDataList);
        if (changed && undoRedo != null && !undoRedo.IsUndoingOrRedoing)
        {
            undoRedo.Add(new MultiEntityTransformAction(oldDataList, newDataList));
        }
        if (changed) MarkProjectDirty();

        // Mirror the C# transform into the native engine so the render
        // surface reflects the new value immediately. Without this the
        // gizmo would update on the next SceneSynced() tick (which only
        // fires on ActiveScene changes), leaving the viewport showing
        // the stale position until the user clicks away and back.
        PushTransformsToNative();
    }

    private void PushTransformsToNative()
    {
        var selected = _msEntity.SelectedEntities;
        if (selected.Count == 0) return;
        if (PushTransformsToNativeCallback != null)
        {
            PushTransformsToNativeCallback(selected);
            return;
        }
        if (!NativeEngineBridge.IsLoaded) return;
        // Walk the same MainWindow data-context chain as TryGetUndoRedo
        // so we hit the same WorldEditorViewModel instance the rest of
        // the editor talks to. There's no cross-view-model service
        // locator in the editor yet, so this duplicated lookup is the
        // simplest reliable path.
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var worldEditor = lifetime?.MainWindow?.DataContext is MainWindowViewModel mainVm
            ? mainVm.WorldEditor
            : null;
        if (worldEditor == null) return;
        foreach (var entity in selected)
        {
            var t = entity.GetTransform();
            if (t == null) continue;
            var nativeId = worldEditor.GetNativeEntityId(entity);
            if (nativeId == 0) continue;
            float[] pos = { t.PositionX, t.PositionY, t.PositionZ };
            float[] rot = { t.RotationX, t.RotationY, t.RotationZ };
            float[] scl = { t.ScaleX, t.ScaleY, t.ScaleZ };
            NativeEngineBridge.SetEntityTransform(nativeId, pos, rot, scl);
        }
    }

    private void MarkProjectDirty()
    {
        if (MarkProjectDirtyCallback != null)
        {
            MarkProjectDirtyCallback();
            return;
        }
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
        if (UndoRedoQuery != null) return UndoRedoQuery();
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is MainWindowViewModel mainVm)
        {
            return mainVm.WorldEditor.UndoRedo;
        }
        return null;
    }

    /// <summary>
    /// Re-fire PropertyChanged for the 9 transform properties so the
    /// inspector TextBoxes re-read from the MSTransform wrapper.
    /// Called by the gizmo drag handler after it mutates the
    /// underlying Transform component in place.
    /// </summary>
    public void RefreshFromTransform(Components.Transform t)
    {
        if (_msEntity.SelectedEntities.Count != 1) return;
        if (!ReferenceEquals(_msEntity.SelectedEntities[0].GetTransform(), t)) return;
        // Re-pull the backing values from the underlying Transform so
        // the panel reflects the new state. Without this the panel
        // shows the pre-gizmo values until the user re-selects the
        // entity (which rebuilds MSTransform and incidentally refreshes).
        Transform?.Refresh();
        NotifyTransformPropertiesChanged();
    }

    private void NotifyTransformPropertiesChanged()
    {
        // PropertiesView binds to these forwarding properties rather than to
        // MSTransform directly. Refreshing MSTransform alone therefore cannot
        // make Avalonia re-read the values after an external mutation such as
        // a gizmo drag.
        OnPropertyChanged(nameof(PosX));   OnPropertyChanged(nameof(PosY));   OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX));   OnPropertyChanged(nameof(RotY));   OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX)); OnPropertyChanged(nameof(ScaleY)); OnPropertyChanged(nameof(ScaleZ));
    }

    private void UpdateFromEntities()
    {
        OnPropertyChanged(nameof(EntityName));
        OnPropertyChanged(nameof(IsEnabled));
        NotifyTransformPropertiesChanged();

        Light?.Refresh();
        Camera?.Refresh();
        MeshRenderer?.Refresh();
        OnPropertyChanged(nameof(Light));
        OnPropertyChanged(nameof(Camera));
        OnPropertyChanged(nameof(MeshRenderer));
        OnPropertyChanged(nameof(HasCameraComponent));

        MixedValueMessage = _msEntity.SelectedEntities.Count > 1
            ? "Multiple entities selected - showing first value or '-' for mixed"
            : string.Empty;
    }
}
