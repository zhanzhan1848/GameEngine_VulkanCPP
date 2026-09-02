using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Primal.CliFramework;

/// <summary>
/// Right-handed, +Y-up 3-component vector. Converts leniently for LLM
/// callers: accepts [1,0,3] or "1,0,3" (宽进); always serializes as a
/// 3-number array (严出).
/// </summary>
[JsonConverter(typeof(Vec3JsonConverter))]
public readonly struct Vec3
{
    public float X { get; }
    public float Y { get; }
    public float Z { get; }

    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }

    public float[] ToArray() => new[] { X, Y, Z };

    public override string ToString() => $"{X},{Y},{Z}";
}

public sealed class Vec3JsonConverter : JsonConverter<Vec3>
{
    public override Vec3 Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        switch (reader.TokenType)
        {
            case JsonTokenType.StartArray:
            {
                var values = new float[3];
                for (int i = 0; i < 3; i++)
                {
                    if (!reader.Read() || reader.TokenType != JsonTokenType.Number)
                        throw new JsonException("vec3 需要 3 个数字，例如 [1,0,3]");
                    values[i] = reader.GetSingle();
                }
                if (reader.Read() && reader.TokenType != JsonTokenType.EndArray)
                    throw new JsonException("vec3 数组长度必须为 3");
                return new Vec3(values[0], values[1], values[2]);
            }
            case JsonTokenType.String:
            {
                var parts = reader.GetString()!.Split(',');
                if (parts.Length != 3)
                    throw new JsonException("vec3 字符串必须是 \"x,y,z\"");
                return new Vec3(
                    float.Parse(parts[0]),
                    float.Parse(parts[1]),
                    float.Parse(parts[2]));
            }
            default:
                throw new JsonException("vec3 接受 [x,y,z] 数组或 \"x,y,z\" 字符串");
        }
    }

    public override void Write(Utf8JsonWriter writer, Vec3 value, JsonSerializerOptions options)
    {
        writer.WriteStartArray();
        writer.WriteNumberValue(value.X);
        writer.WriteNumberValue(value.Y);
        writer.WriteNumberValue(value.Z);
        writer.WriteEndArray();
    }
}
