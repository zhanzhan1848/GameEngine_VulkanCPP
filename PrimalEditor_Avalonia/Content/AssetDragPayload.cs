using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content;

// Drag payload uses a JSON-serialized record rather than a live AssetInfo
// so the drop target can rebuild what it needs from the registry without
// depending on the source panel's instance identity.
public sealed record AssetDragPayload(
    Guid Guid,
    string FullPath,
    bool IsContainer,
    Guid? ParentGuid)
{
    // Shared between ContentBrowser (drag source) and WorldEditor (drop target).
    public const string DataFormat = "PrimalEditor.AssetDrag";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        IncludeFields = false,
        PropertyNamingPolicy = null,
    };

    public string Serialize() => JsonSerializer.Serialize(this, JsonOptions);

    public static AssetDragPayload? Deserialize(string raw)
    {
        if (string.IsNullOrEmpty(raw)) return null;
        try
        {
            return JsonSerializer.Deserialize<AssetDragPayload>(raw, JsonOptions);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}