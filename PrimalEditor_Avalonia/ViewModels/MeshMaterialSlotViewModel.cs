using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Read-only presentation model for one MeshRenderer material slot. Changes
/// are routed through PropertiesViewModel so multi-selection, dirty state and
/// the native viewport are updated together.
/// </summary>
public sealed class MeshMaterialSlotViewModel
{
    public int Index { get; }
    public string SlotName => $"Element {Index}";
    public AssetGuid MaterialGuid { get; }
    public string DisplayName { get; }
    public bool HasMaterial => MaterialGuid.Value != System.Guid.Empty;

    public MeshMaterialSlotViewModel(int index, AssetGuid materialGuid)
    {
        Index = index;
        MaterialGuid = materialGuid;
        var asset = materialGuid.Value == System.Guid.Empty
            ? null
            : AssetRegistry.GetByGuid(materialGuid.Value);
        DisplayName = asset?.FileName ?? (HasMaterial ? materialGuid.Value.ToString("D") : "Drop material here");
    }
}
