using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Content.Metadata;

public sealed class AssetMetaDocument
{
    public const int CurrentVersion = 1;

    public int MetaVersion { get; set; } = CurrentVersion;

    [JsonConverter(typeof(AssetGuid.JsonConverter))]
    public AssetGuid Guid { get; set; }

    public string Importer { get; set; } = string.Empty;
    public JsonObject Settings { get; set; } = new();

    public bool IsValid =>
        MetaVersion == CurrentVersion &&
        Guid.Value != System.Guid.Empty &&
        !string.IsNullOrWhiteSpace(Importer) &&
        Settings is not null;
}
