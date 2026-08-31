using CommunityToolkit.Mvvm.ComponentModel;

namespace PrimalEditor_Avalonia.Editors.Material;

/// <summary>
/// One entry in the left palette of the material editor. Populated once
/// from <see cref="Native.MaterialGraph.MaterialGraphReflectionNative"/>
/// when the editor opens.
/// </summary>
public sealed class MaterialNodeTemplateViewModel : ObservableObject
{
    public string TypeName { get; init; } = string.Empty;
    public string DisplayName { get; init; } = string.Empty;
    public string Category { get; init; } = string.Empty;
    public bool IsTerminal { get; init; }
}