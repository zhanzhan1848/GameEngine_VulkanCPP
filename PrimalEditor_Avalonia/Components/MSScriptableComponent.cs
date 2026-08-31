using System;
using System.Linq;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Multi-selection wrapper for <see cref="ScriptableComponent"/>s of the
/// same concrete type. Mixed-type multi-selection is not yet supported
/// (the inspector picks the first entity's component when the user
/// selects multiple entities of differing script types).
/// </summary>
public sealed class MSScriptableComponent : ViewModelBase, IMSComponent
{
    private readonly MSEntity _msEntity;
    private readonly ScriptableComponent _first;
    public System.Collections.Generic.List<ScriptableComponent> SelectedComponents { get; }

    public string ScriptTypeId { get; }

    public string? ScriptEnabledHint { get; private set; }

    public MSScriptableComponent(MSEntity msEntity, ScriptableComponent first)
    {
        _msEntity = msEntity;
        _first = first;
        SelectedComponents = msEntity.SelectedEntities
            .Select(e => e.GetComponent<ScriptableComponent>())
            .Where(c => c != null && c.ScriptTypeId == first.ScriptTypeId)
            .Cast<ScriptableComponent>()
            .ToList();
        ScriptTypeId = first.ScriptTypeId;
        Refresh();
    }

    public void Refresh()
    {
        ScriptEnabledHint = SelectedComponents.Count == 0
            ? null
            : $"Attached to {SelectedComponents.Count} entity(ies) ({ScriptTypeId})";
        OnPropertyChanged(nameof(ScriptEnabledHint));
    }
}