namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Editor-side marker for an "environment light": selecting the entity
/// surfaces the scene environment panel (HDR env map, skybox, tone mapping)
/// in the inspector. Purely editor-level — no engine component is created;
/// the settings live on the scene's <c>SceneEnvironment</c>, so every scene
/// has exactly one environment regardless of how many Env Light entities
/// reference it.
/// </summary>
public sealed class EnvironmentComponent : Component
{
    // No multiselection editor exists for this marker component; the
    // environment panel is scene-level state, not per-entity state.
    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) => null!;
}
