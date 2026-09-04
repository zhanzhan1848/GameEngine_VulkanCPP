using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject.Documents;

/// <summary>
/// A8 场景 JSON 排序稳定化：Dictionary&lt;string, TValue&gt; 字段的写盘键序固定为
/// Ordinal 排序。System.Text.Json 按枚举序写 Dictionary，而枚举序跟随插入序 ——
/// Material 覆盖表 / Prefab overrides / 脚本参数这类"编辑顺序即写盘顺序"的字段，
/// 两个用户以不同顺序做出相同逻辑修改就会产生假 git diff。读侧保持普通字典语义。
/// 应用方式：目标属性上加 <c>[JsonConverter(typeof(SortedStringKeysConverter&lt;TValue&gt;))]</c>。
/// </summary>
public sealed class SortedStringKeysConverter<TValue> : JsonConverter<Dictionary<string, TValue>>
{
    public override Dictionary<string, TValue> Read(
        ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException($"Expected object for Dictionary<string,{typeof(TValue).Name}>");

        var dict = new Dictionary<string, TValue>();
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.EndObject) return dict;
            var key = reader.GetString() ?? string.Empty;
            reader.Read();
            var value = JsonSerializer.Deserialize<TValue>(ref reader, options);
            dict[key] = value!;
        }
        throw new JsonException("Truncated object in SortedStringKeysConverter.Read");
    }

    public override void Write(
        Utf8JsonWriter writer, Dictionary<string, TValue> value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        foreach (var kv in value.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            writer.WritePropertyName(kv.Key);
            JsonSerializer.Serialize(writer, kv.Value, options);
        }
        writer.WriteEndObject();
    }
}
