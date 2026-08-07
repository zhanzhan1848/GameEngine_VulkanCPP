using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class ImportRequest
{
    public string SourcePath { get; init; } = string.Empty;
    public string DestinationDir { get; init; } = string.Empty;
    public IAssetImportSettings Settings { get; init; } = null!;
    public string? Subfolder { get; init; }
}
