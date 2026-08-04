using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.GameProject.Documents;

public sealed class SceneDocument
{
    public const int CurrentSchemaVersion = 2;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;

    [JsonPropertyName("$id")]
    [JsonConverter(typeof(SceneGuid.JsonConverter))]
    public SceneGuid SceneGuid { get; set; } = new SceneGuid(Guid.NewGuid());

    [JsonPropertyName("Name")]
    public string Name { get; set; } = "New Scene";

    [JsonPropertyName("Entities")]
    public List<EntityDocument> Entities { get; set; } = new();

    [JsonIgnore]
    public bool IsValid =>
        SchemaVersion == CurrentSchemaVersion &&
        SceneGuid.Value != Guid.Empty &&
        !string.IsNullOrWhiteSpace(Name);

    public static JsonSerializerOptions JsonOptions { get; } = CreateOptions();

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions { WriteIndented = true };
        options.Converters.Add(new SceneGuid.JsonConverter());
        options.Converters.Add(new EntityGuid.JsonConverter());
        options.Converters.Add(new AssetGuid.JsonConverter());
        return options;
    }
}