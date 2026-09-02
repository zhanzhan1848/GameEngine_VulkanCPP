using System.Text.Json;
using System.Text.Json.Serialization;

namespace Primal.CliFramework;

/// <summary>
/// Wire protocol (transport-agnostic, JSON-RPC style). One frame =
/// 4-byte little-endian length prefix + UTF-8 JSON payload.
///   → {"id":"42","verb":"entity.set-transform","params":{...}}
///   ← {"id":"42","ok":true,"data":{...},"meta":{"queueMs":1.2,"execMs":0.4}}
///   ← {"id":"42","ok":false,"error":{"code":"NOT_FOUND","message":"...","hint":"..."}}
/// Params ride as a raw JsonElement so the registry binds per-parameter and
/// can produce precise "missing parameter X" errors instead of one opaque
/// deserialization failure.
/// </summary>
public sealed class CliRequest
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("verb")]
    public string Verb { get; set; } = "";

    [JsonPropertyName("params")]
    public JsonElement Params { get; set; } = default;

    [JsonIgnore]
    public bool HasParams => Params.ValueKind is JsonValueKind.Object or JsonValueKind.Array;
}

public sealed class CliErrorPayload
{
    [JsonPropertyName("code")]
    public string Code { get; set; } = CliErrorCodes.Internal;

    [JsonPropertyName("message")]
    public string Message { get; set; } = "";

    [JsonPropertyName("hint")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Hint { get; set; }
}

public sealed class CliResponse
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("ok")]
    public bool Ok { get; set; }

    [JsonPropertyName("data")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public JsonElement? Data { get; set; }

    [JsonPropertyName("error")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public CliErrorPayload? Error { get; set; }

    [JsonPropertyName("meta")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public CliResponseMeta? Meta { get; set; }
}

/// <summary>Queue + execution latency, so agent callers can tell a busy UI
/// thread from a slow command.</summary>
public sealed class CliResponseMeta
{
    [JsonPropertyName("queueMs")]
    public double QueueMs { get; set; }

    [JsonPropertyName("execMs")]
    public double ExecMs { get; set; }
}
