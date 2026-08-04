using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Components;

public class MSMaterialOverride : MSComponentBase<MaterialOverrideComponent>
{
    public MSMaterialOverride(MSEntity msEntity) : base(msEntity) { }

    /// <summary>
    /// Read-only snapshot of all materials across the selection. The
    /// canonical list is taken from the first selected entity — the
    /// inspector edits that and <see cref="ApplyToSelection"/> pushes
    /// the change back to every selected entity.
    /// </summary>
    public IReadOnlyList<MaterialSlot> Materials
    {
        get => _materials;
        private set { _materials = value.ToList(); OnPropertyChanged(); }
    }
    private List<MaterialSlot> _materials = new();

    protected override bool UpdateMSComponent()
    {
        RebuildMaterials();
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        // Material slots are mutated by ApplyToSelection, which goes
        // through MaterialOverrideViewModel — the inspector is not
        // two-way bound directly into the MS component. UpdateComponents
        // never fires from XAML.
        return true;
    }

    /// <summary>
    /// Apply a GUID to the slot at <paramref name="index"/> on every
    /// selected entity. Returns the number of entities whose slot was
    /// mutated; mismatched-length entities are skipped so the user
    /// gets a "first entity's count is canonical" warning instead of a
    /// crash.
    /// </summary>
    public int ApplyToSelection(int index, AssetGuid guid)
    {
        if (SelectedComponents.Count == 0) return 0;
        var first = SelectedComponents[0];
        if (index < 0 || index >= first.Slots.Count) return 0;

        var mutated = 0;
        foreach (var component in SelectedComponents)
        {
            if (index >= component.Slots.Count) continue;
            component.Slots[index].MaterialGuid = guid;
            mutated++;
        }
        RebuildMaterials();
        return mutated;
    }

    private void RebuildMaterials()
    {
        if (SelectedComponents.Count == 0)
        {
            Materials = Array.Empty<MaterialSlot>();
            return;
        }
        var first = SelectedComponents[0];
        var slot = first.Slots.FirstOrDefault();
        Materials = slot != null
            ? new List<MaterialSlot> { new MaterialSlot(slot) }
            : Array.Empty<MaterialSlot>().ToList();
    }
}

/// <summary>
/// Snapshot of a single MaterialOverrideSlot for inspector binding.
/// The slot itself is mutable; <see cref="MSMaterialOverride.ApplyToSelection"/>
/// is the canonical write-back path used by the picker button and
/// any "edit slot N" UI actions.
/// </summary>
public sealed class MaterialSlot
{
    public MaterialSlot(MaterialOverrideSlot slot)
    {
        SourceSlot = slot ?? throw new ArgumentNullException(nameof(slot));
    }

    public MaterialOverrideSlot SourceSlot { get; }

    public string SlotName => SourceSlot.Slot;
    public AssetGuid MaterialGuid => SourceSlot.MaterialGuid;
}
