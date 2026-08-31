using System;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Wraps a single <see cref="MaterialOverrideSlot"/> for the Properties
/// panel so the XAML can bind to plain properties instead of poking at
/// the slot's fields directly. The base class handles change
/// notification.
/// </summary>
public sealed class MaterialOverrideViewModel : ViewModelBase
{
    private readonly MaterialOverrideSlot _slot;

    public MaterialOverrideViewModel(MaterialOverrideSlot slot, int index)
    {
        _slot = slot ?? throw new ArgumentNullException(nameof(slot));
        Index = index;
    }

    public int Index { get; }

    public string SlotName
    {
        get => _slot.Slot;
        set
        {
            if (_slot.Slot == value) return;
            _slot.Slot = value ?? string.Empty;
            OnPropertyChanged();
        }
    }

    /// <summary>Submesh the override targets (-1 means whole mesh).
    /// The inspector binds this so multi-submesh meshes can override
    /// per-submesh without ambiguity.</summary>
    public int SubMeshIndex
    {
        get => _slot.SubMeshIndex;
        set
        {
            if (_slot.SubMeshIndex == value) return;
            _slot.SubMeshIndex = value;
            OnPropertyChanged();
        }
    }

    /// <summary>
    /// Render-time helper: the slot's material Guid as a string for
    /// XAML binding. Empty Guid shows up as the all-zero guid. The
    /// editor layer never compares to this — it round-trips through
    /// <see cref="MaterialOverrideSlot.MaterialGuid"/>.
    /// </summary>
    public string MaterialGuid
    {
        get => _slot.MaterialGuid.Value.ToString("D");
        set
        {
            if (!System.Guid.TryParse(value, out var parsed)) return;
            if (_slot.MaterialGuid.Value == parsed) return;
            _slot.MaterialGuid = new PrimalEditor_Avalonia.Content.AssetGuid(parsed);
            OnPropertyChanged();
        }
    }

    /// <summary>Number of parameter overrides on this slot — surfaced
    /// in the inspector so the user can expand the parameter grid.</summary>
    public int ParameterCount => _slot.Parameters?.Count ?? 0;
}
