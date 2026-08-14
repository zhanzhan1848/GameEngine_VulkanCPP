using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Documents;

public sealed class EditorCameraDocument
{
    [JsonPropertyName("TargetX")] public float TargetX { get; set; }
    [JsonPropertyName("TargetY")] public float TargetY { get; set; }
    [JsonPropertyName("TargetZ")] public float TargetZ { get; set; }
    [JsonPropertyName("Distance")] public float Distance { get; set; } = 12f;
    [JsonPropertyName("Yaw")] public float Yaw { get; set; }
    [JsonPropertyName("Pitch")] public float Pitch { get; set; } = EditorCameraState.DefaultPitch;
}
