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

    public GameEntity[] Entities
    {
        set
        {
            var ms = new MSEntity();
            if (value != null)
                foreach (var e in value) ms.SelectedEntities.Add(e);
            MSEntity = ms;
            Transform = ms.SelectedEntities.Count > 0 ? new MSTransform(ms) : null;
            UpdateFromEntities();
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

    public float? PosX   { get => Transform?.PosX;   set { if (Transform != null) { Transform.PosX   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? PosY   { get => Transform?.PosY;   set { if (Transform != null) { Transform.PosY   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? PosZ   { get => Transform?.PosZ;   set { if (Transform != null) { Transform.PosZ   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? RotX   { get => Transform?.RotX;   set { if (Transform != null) { Transform.RotX   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? RotY   { get => Transform?.RotY;   set { if (Transform != null) { Transform.RotY   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? RotZ   { get => Transform?.RotZ;   set { if (Transform != null) { Transform.RotZ   = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? ScaleX { get => Transform?.ScaleX; set { if (Transform != null) { Transform.ScaleX = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? ScaleY { get => Transform?.ScaleY; set { if (Transform != null) { Transform.ScaleY = value; OnPropertyChanged(); RecordUndo(); } } }
    public float? ScaleZ { get => Transform?.ScaleZ; set { if (Transform != null) { Transform.ScaleZ = value; OnPropertyChanged(); RecordUndo(); } } }

    private void RecordUndo()
    {
        var undoRedo = TryGetUndoRedo();
        if (undoRedo == null || Transform == null) return;
        var oldDataList = new List<(GameEntity entity, TransformData data)>();
        var newDataList = new List<(GameEntity entity, TransformData data)>();
        foreach (var e in _msEntity.SelectedEntities)
        {
            var t = e.GetTransform();
            if (t == null) continue;
            oldDataList.Add((e, TransformData.FromTransform(t)));
        }
        foreach (var e in _msEntity.SelectedEntities)
        {
            var t = e.GetTransform();
            if (t == null) continue;
            newDataList.Add((e, TransformData.FromTransform(t)));
        }
        if (oldDataList.Count > 0)
            undoRedo.Add(new MultiEntityTransformAction(oldDataList, newDataList));
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

        MixedValueMessage = _msEntity.SelectedEntities.Count > 1
            ? "Multiple entities selected - showing first value or '-' for mixed"
            : string.Empty;
    }
}