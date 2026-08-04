using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Identifiers;

public readonly struct ProjectGuid : IEquatable<ProjectGuid>
{
    public Guid Value { get; }

    public ProjectGuid(Guid value)
    {
        if (value == Guid.Empty)
            throw new ArgumentException("ProjectGuid cannot be empty", nameof(value));

        Value = value;
    }

    public static ProjectGuid Parse(string value)
    {
        var guid = Guid.Parse(value);
        if (guid == Guid.Empty)
            throw new ArgumentException("ProjectGuid cannot be empty", nameof(value));

        return new ProjectGuid(guid);
    }

    public bool Equals(ProjectGuid other)
    {
        return Value == other.Value;
    }

    public override bool Equals(object? obj)
    {
        return obj is ProjectGuid other && Equals(other);
    }

    public override int GetHashCode()
    {
        return Value.GetHashCode();
    }

    public static bool operator ==(ProjectGuid left, ProjectGuid right)
    {
        return left.Equals(right);
    }

    public static bool operator !=(ProjectGuid left, ProjectGuid right)
    {
        return !left.Equals(right);
    }

    public class JsonConverter : JsonConverter<ProjectGuid>
    {
        public override ProjectGuid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var value = reader.GetString();
            return Parse(value!);
        }

        public override void Write(Utf8JsonWriter writer, ProjectGuid value, JsonSerializerOptions options)
        {
            writer.WriteStringValue(value.Value.ToString("D").ToLowerInvariant());
        }
    }
}