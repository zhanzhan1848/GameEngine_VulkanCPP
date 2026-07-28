using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

[JsonDerivedType(typeof(Transform), typeDiscriminator: "transform")]
public abstract class Component : ViewModelBase
{
    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled { get; set; } = true;

    public abstract IMSComponent GetMultiselectionComponent(MSEntity msEntity);
}