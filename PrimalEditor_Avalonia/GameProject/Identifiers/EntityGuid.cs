using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Identifiers;

public readonly struct EntityGuid : IEquatable<EntityGuid>
{
    public Guid Value { get; }

    public EntityGuid(Guid value)
    {
        if (value == Guid.Empty)
            throw new ArgumentException("EntityGuid cannot be empty", nameof(value));

        Value = value;
    }

    public static EntityGuid Parse(string value)
    {
        var guid = Guid.Parse(value);
        if (guid == Guid.Empty)
            throw new ArgumentException("EntityGuid cannot be empty", nameof(value));

        return new EntityGuid(guid);
    }

    public bool Equals(EntityGuid other)
    {
        return Value == other.Value;
    }

    public override bool Equals(object? obj)
    {
        return obj is EntityGuid other && Equals(other);
    }

    public override int GetHashCode()
    {
        return Value.GetHashCode();
    }

    public static bool operator ==(EntityGuid left, EntityGuid right)
    {
        return left.Equals(right);
    }

    public static bool operator !=(EntityGuid left, EntityGuid right)
    {
        return !left.Equals(right);
    }

    public class JsonConverter : JsonConverter<EntityGuid>
    {
        public override EntityGuid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var value = reader.GetString();
            return Parse(value!);
        }

        public override void Write(Utf8JsonWriter writer, EntityGuid value, JsonSerializerOptions options)
        {
            writer.WriteStringValue(value.Value.ToString("D").ToLowerInvariant());
        }
    }
}