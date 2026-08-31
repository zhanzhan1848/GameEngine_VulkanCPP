using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native.Importers;

public interface IAssetImporterPlugin
{
    string DisplayName { get; }
    IReadOnlyList<AssetType> HandledTypes { get; }
    Type SettingsType { get; }
    bool CanClaim(string filePath);
    /// Build a panel control hosting the per-category settings UI plus its
    /// bound ViewModel. The dialog binds ContentControl.Content to whatever
    /// this returns.
    UserControl CreatePanel(IAssetImportSettings defaults);
    Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct);
}

