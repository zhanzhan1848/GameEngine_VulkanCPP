using System.Collections.ObjectModel;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Procedural geometry that is available in every editor project and does
/// not require an asset on disk. The value is persisted with the scene.
/// </summary>
public enum BuiltinMeshKind
{
    None,
    Sphere,
    Box,
    Plane,
    Capsule,
}

public sealed class MeshRendererComponent : Component
{
    public AssetGuid MeshGuid { get; set; }
    public BuiltinMeshKind BuiltinMesh { get; set; }
    public ObservableCollection<AssetGuid> MaterialGuids { get; set; } = new();
    public bool CastShadow { get; set; } = true;
    public bool ReceiveShadow { get; set; } = true;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSMeshRenderer(msEntity);
}
