using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class PropertiesViewModel : ViewModelBase
{
    private GameEntity[] _entities = System.Array.Empty<GameEntity>();
    private string _mixedValueMessage = string.Empty;

    public bool HasSelection => _entities.Length > 0;
    public bool HasMultipleSelection => _entities.Length > 1;
    public bool HasMixedValues => !string.IsNullOrEmpty(_mixedValueMessage);

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

    public GameEntity[] Entities
    {
        get => _entities;
        set
        {
            if (_entities != value)
            {
                _entities = value ?? System.Array.Empty<GameEntity>();
                OnPropertyChanged(nameof(Entities));
                OnPropertyChanged(nameof(HasSelection));
                OnPropertyChanged(nameof(HasMultipleSelection));
                UpdateFromEntities();
            }
        }
    }

    public string EntityName
    {
        get => _entities.Length == 1 ? _entities[0].Name : string.Empty;
        set
        {
            if (_entities.Length == 1 && _entities[0].Name != value)
            {
                _entities[0].Name = value;
                OnPropertyChanged(nameof(EntityName));
            }
        }
    }

    public bool IsEnabled
    {
        get => _entities.Length > 0 && _entities.All(e => e.IsEnabled);
        set
        {
            foreach (var entity in _entities)
            {
                entity.IsEnabled = value;
            }
            OnPropertyChanged(nameof(IsEnabled));
        }
    }

    public string PosX
    {
        get => GetTransformValue(t => t.PositionX);
        set => SetTransformValue(t => t.PositionX = float.Parse(value), t => t.PositionX);
    }

    public string PosY
    {
        get => GetTransformValue(t => t.PositionY);
        set => SetTransformValue(t => t.PositionY = float.Parse(value), t => t.PositionY);
    }

    public string PosZ
    {
        get => GetTransformValue(t => t.PositionZ);
        set => SetTransformValue(t => t.PositionZ = float.Parse(value), t => t.PositionZ);
    }

    public string RotX
    {
        get => GetTransformValue(t => t.RotationX);
        set => SetTransformValue(t => t.RotationX = float.Parse(value), t => t.RotationX);
    }

    public string RotY
    {
        get => GetTransformValue(t => t.RotationY);
        set => SetTransformValue(t => t.RotationY = float.Parse(value), t => t.RotationY);
    }

    public string RotZ
    {
        get => GetTransformValue(t => t.RotationZ);
        set => SetTransformValue(t => t.RotationZ = float.Parse(value), t => t.RotationZ);
    }

    public string ScaleX
    {
        get => GetTransformValue(t => t.ScaleX);
        set => SetTransformValue(t => t.ScaleX = float.Parse(value), t => t.ScaleX);
    }

    public string ScaleY
    {
        get => GetTransformValue(t => t.ScaleY);
        set => SetTransformValue(t => t.ScaleY = float.Parse(value), t => t.ScaleY);
    }

    public string ScaleZ
    {
        get => GetTransformValue(t => t.ScaleZ);
        set => SetTransformValue(t => t.ScaleZ = float.Parse(value), t => t.ScaleZ);
    }

    private string GetTransformValue(System.Func<Transform, float> selector)
    {
        if (_entities.Length == 0) return "0";
        var transforms = _entities.Select(e => e.Components.OfType<Transform>().FirstOrDefault())
            .Where(t => t != null).Cast<Transform>().ToList();
        if (transforms.Count == 0) return "0";
        var first = selector(transforms[0]);
        return transforms.All(t => System.Math.Abs(selector(t) - first) < 0.0001f)
            ? first.ToString("F3")
            : "-";
    }

    private void SetTransformValue(System.Action<Transform> setter, System.Func<Transform, float> getter)
    {
        var oldDataList = new System.Collections.Generic.List<(Components.GameEntity entity, PrimalEditor_Avalonia.Common.TransformData data)>();
        foreach (var entity in _entities)
        {
            var transform = entity.Components.OfType<Transform>().FirstOrDefault();
            if (transform != null)
            {
                oldDataList.Add((entity, PrimalEditor_Avalonia.Common.TransformData.FromTransform(transform)));
            }
        }

        foreach (var entity in _entities)
        {
            var transform = entity.Components.OfType<Transform>().FirstOrDefault();
            if (transform != null)
            {
                setter(transform);
            }
        }

        var undoRedo = TryGetUndoRedo();
        if (undoRedo != null && oldDataList.Count > 0)
        {
            var newDataList = new System.Collections.Generic.List<(Components.GameEntity entity, PrimalEditor_Avalonia.Common.TransformData data)>();
            foreach (var entity in _entities)
            {
                var transform = entity.Components.OfType<Transform>().FirstOrDefault();
                if (transform != null)
                {
                    newDataList.Add((entity, PrimalEditor_Avalonia.Common.TransformData.FromTransform(transform)));
                }
            }

            var batch = new PrimalEditor_Avalonia.Common.MultiEntityTransformAction(oldDataList, newDataList);
            undoRedo.Add(batch);
        }
    }

    private PrimalEditor_Avalonia.Common.UndoRedo? TryGetUndoRedo()
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
        OnPropertyChanged(nameof(PosX));
        OnPropertyChanged(nameof(PosY));
        OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX));
        OnPropertyChanged(nameof(RotY));
        OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX));
        OnPropertyChanged(nameof(ScaleY));
        OnPropertyChanged(nameof(ScaleZ));

        if (_entities.Length > 1)
        {
            MixedValueMessage = "Multiple entities selected - showing first value or '-' for mixed";
        }
        else
        {
            MixedValueMessage = string.Empty;
        }
    }
}