using System.Collections.Generic;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Base for data-only "scripts" attached to entities. Round-trips
/// through JSON with a stable <see cref="ScriptTypeId"/> discriminator;
/// the runtime engine does not execute anything yet — that's reserved
/// for the script runtime. Two concrete subclasses today:
/// <see cref="RotateScriptComponent"/> and <see cref="PatrolScriptComponent"/>;
/// add more by deriving and registering via <c>[JsonDerivedType]</c>.
/// </summary>
public abstract class ScriptableComponent : Component
{
    [JsonPropertyName("ScriptTypeId")]
    public abstract string ScriptTypeId { get; }

    [JsonPropertyName("Parameters")]
    [System.Text.Json.Serialization.JsonConverter(
        typeof(PrimalEditor_Avalonia.GameProject.Documents.SortedStringKeysConverter<string>))]
    public Dictionary<string, string> Parameters { get; set; } = new();

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity)
        => new MSScriptableComponent(msEntity, this);
}

[JsonDerivedType(typeof(RotateScriptComponent), typeDiscriminator: "scriptRotate")]
public class RotateScriptComponent : ScriptableComponent
{
    public override string ScriptTypeId => "Rotate";

    [JsonPropertyName("SpeedX")] public string SpeedX { get; set; } = "0";
    [JsonPropertyName("SpeedY")] public string SpeedY { get; set; } = "45";
    [JsonPropertyName("SpeedZ")] public string SpeedZ { get; set; } = "0";
}

[JsonDerivedType(typeof(PatrolScriptComponent), typeDiscriminator: "scriptPatrol")]
public class PatrolScriptComponent : ScriptableComponent
{
    public override string ScriptTypeId => "Patrol";

    /// <summary>Comma-separated x,y,z waypoints, semicolon-delimited.</summary>
    [JsonPropertyName("Path")] public string Path { get; set; } = "0,0,0;5,0,0;5,0,5";
    [JsonPropertyName("Loop")] public bool Loop { get; set; } = true;
}