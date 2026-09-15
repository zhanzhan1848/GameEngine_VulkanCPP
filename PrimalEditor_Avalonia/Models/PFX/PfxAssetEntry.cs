using System.IO;

namespace PrimalEditor_Avalonia.Models.PFX;

/// <summary>发射器浏览器资产行。</summary>
public sealed class PfxAssetEntry
{
    public string FullPath { get; init; } = "";
    public string DisplayName => Path.GetFileNameWithoutExtension(FullPath);
    public string Directory => Path.GetDirectoryName(FullPath) ?? "";
}
