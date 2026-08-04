using System.Collections.ObjectModel;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Components;

public sealed class MeshRendererComponent : Component
{
    public AssetGuid MeshGuid { get; set; }
    public ObservableCollection<AssetGuid> MaterialGuids { get; set; } = new();
    public bool CastShadow { get; set; } = true;
    public bool ReceiveShadow { get; set; } = true;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSMeshRenderer(msEntity);
}
