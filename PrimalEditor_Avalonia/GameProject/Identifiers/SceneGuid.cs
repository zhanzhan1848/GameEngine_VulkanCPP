using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Identifiers;

public readonly struct SceneGuid : IEquatable<SceneGuid>
{
    public Guid Value { get; }

    public SceneGuid(Guid value)
    {
        if (value == Guid.Empty)
            throw new ArgumentException("SceneGuid cannot be empty", nameof(value));

        Value = value;
    }

    public static SceneGuid Parse(string value)
    {
        var guid = Guid.Parse(value);
        if (guid == Guid.Empty)
            throw new ArgumentException("SceneGuid cannot be empty", nameof(value));

        return new SceneGuid(guid);
    }

    public bool Equals(SceneGuid other)
    {
        return Value == other.Value;
    }

    public override bool Equals(object? obj)
    {
        return obj is SceneGuid other && Equals(other);
    }

    public override int GetHashCode()
    {
        return Value.GetHashCode();
    }

    public static bool operator ==(SceneGuid left, SceneGuid right)
    {
        return left.Equals(right);
    }

    public static bool operator !=(SceneGuid left, SceneGuid right)
    {
        return !left.Equals(right);
    }

    public class JsonConverter : JsonConverter<SceneGuid>
    {
        public override SceneGuid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var value = reader.GetString();
            return Parse(value!);
        }

        public override void Write(Utf8JsonWriter writer, SceneGuid value, JsonSerializerOptions options)
        {
            writer.WriteStringValue(value.Value.ToString("D").ToLowerInvariant());
        }
    }
}