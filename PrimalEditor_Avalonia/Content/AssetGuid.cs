using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content;

public readonly struct AssetGuid : IEquatable<AssetGuid>
{
    public Guid Value { get; }

    public AssetGuid(Guid value)
    {
        Value = value;
    }

    public static AssetGuid Parse(string value)
    {
        var guid = Guid.Parse(value);
        return new AssetGuid(guid);
    }

    public bool Equals(AssetGuid other)
    {
        return Value == other.Value;
    }

    public override bool Equals(object? obj)
    {
        return obj is AssetGuid other && Equals(other);
    }

    public override int GetHashCode()
    {
        return Value.GetHashCode();
    }

    public static bool operator ==(AssetGuid left, AssetGuid right)
    {
        return left.Equals(right);
    }

    public static bool operator !=(AssetGuid left, AssetGuid right)
    {
        return !left.Equals(right);
    }

    public class JsonConverter : JsonConverter<AssetGuid>
    {
        public override AssetGuid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var value = reader.GetString();
            return Parse(value!);
        }

        public override void Write(Utf8JsonWriter writer, AssetGuid value, JsonSerializerOptions options)
        {
            writer.WriteStringValue(value.Value.ToString("D").ToLowerInvariant());
        }
    }
}