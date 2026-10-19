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

    /// <summary>
    /// Scene-level environment. Optional in JSON — missing field
    /// yields a default-constructed instance. <see cref="IsValid"/>
    /// intentionally ignores this field so old v2 scenes keep loading
    /// after this addition (the schema is unchanged at version 2).
    /// </summary>
    [JsonPropertyName("Environment")]
    public SceneEnvironmentDocument Environment { get; set; } = new();

    [JsonPropertyName("EditorCamera")]
    public EditorCameraDocument EditorCamera { get; set; } = new();

    /// <summary>
    /// Lightmap 页路径（2026-10-19 计划）。可选字段——旧 v2 场景缺失时为
    /// null，IsValid 不检查（与 Environment 同策略，schema 版本不变）。
    /// </summary>
    [JsonPropertyName("LightmapPage")]
    public string? LightmapPage { get; set; }

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
