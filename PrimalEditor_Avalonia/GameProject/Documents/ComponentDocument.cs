using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Documents;

public sealed class ComponentDocument
{
    [JsonPropertyName("$type")]
    public string Type { get; set; } = string.Empty;

    [JsonPropertyName("Data")]
    public JsonElement Data { get; set; }
}