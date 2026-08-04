using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.GameProject.Documents;

public sealed class ProjectDocument
{
    public const int CurrentSchemaVersion = 2;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;

    [JsonConverter(typeof(ProjectGuid.JsonConverter))]
    public ProjectGuid ProjectGuid { get; set; }

    public string Name { get; set; } = "New Project";
    public string BuildConfig { get; set; } = "DebugEditor";

    [JsonConverter(typeof(NullableSceneGuidConverter))]
    public SceneGuid? DefaultScene { get; set; }

    public List<SceneGuid> Scenes { get; set; } = new();
    public RenderSettingsDocument RenderSettings { get; set; } = new();

    [JsonIgnore]
    public bool IsValid =>
        SchemaVersion == CurrentSchemaVersion &&
        ProjectGuid.Value != Guid.Empty &&
        !string.IsNullOrWhiteSpace(Name) &&
        Project.BuildConfigurationNames.Contains(BuildConfig, StringComparer.Ordinal) &&
        (DefaultScene is null || Scenes.Contains(DefaultScene.Value));

    public static JsonSerializerOptions JsonOptions { get; } = CreateOptions();

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions { WriteIndented = true };
        options.Converters.Add(new ProjectGuid.JsonConverter());
        options.Converters.Add(new SceneGuid.JsonConverter());
        options.Converters.Add(new NullableSceneGuidConverter());
        return options;
    }

    private sealed class NullableSceneGuidConverter : JsonConverter<SceneGuid?>
    {
        public override SceneGuid? Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
            reader.TokenType == JsonTokenType.Null ? null : SceneGuid.Parse(reader.GetString() ?? throw new JsonException());

        public override void Write(Utf8JsonWriter writer, SceneGuid? value, JsonSerializerOptions options)
        {
            if (value is null) writer.WriteNullValue();
            else writer.WriteStringValue(value.Value.Value.ToString("D").ToLowerInvariant());
        }
    }
}
