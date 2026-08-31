using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.GameProject.Documents;

public sealed class EntityDocument
{
    [JsonPropertyName("EntityGuid")]
    [JsonConverter(typeof(EntityGuid.JsonConverter))]
    public EntityGuid EntityGuid { get; set; }

    [JsonPropertyName("Name")]
    public string Name { get; set; } = "Entity";

    [JsonPropertyName("ParentEntityGuid")]
    [JsonConverter(typeof(NullableEntityGuidConverter))]
    public EntityGuid? ParentEntityGuid { get; set; }

    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled { get; set; } = true;

    [JsonPropertyName("Components")]
    public List<ComponentDocument> Components { get; set; } = new();

    private sealed class NullableEntityGuidConverter : JsonConverter<EntityGuid?>
    {
        public override EntityGuid? Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
            reader.TokenType == JsonTokenType.Null ? null : EntityGuid.Parse(reader.GetString() ?? throw new JsonException());

        public override void Write(Utf8JsonWriter writer, EntityGuid? value, JsonSerializerOptions options)
        {
            if (value is null) writer.WriteNullValue();
            else writer.WriteStringValue(value.Value.Value.ToString("D").ToLowerInvariant());
        }
    }
}