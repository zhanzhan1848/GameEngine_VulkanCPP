using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

[JsonDerivedType(typeof(Transform), typeDiscriminator: "transform")]
[JsonDerivedType(typeof(MeshRendererComponent), typeDiscriminator: "meshRenderer")]
[JsonDerivedType(typeof(LightComponent), typeDiscriminator: "light")]
[JsonDerivedType(typeof(CameraComponent), typeDiscriminator: "camera")]
[JsonDerivedType(typeof(MaterialOverrideComponent), typeDiscriminator: "materialOverride")]
public abstract class Component : ViewModelBase
{
    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled { get; set; } = true;

    public abstract IMSComponent GetMultiselectionComponent(MSEntity msEntity);
}