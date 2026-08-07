using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native.Importers;

public interface IAssetImporterPlugin
{
    string DisplayName { get; }
    IReadOnlyList<AssetType> HandledTypes { get; }
    Type SettingsType { get; }
    bool CanClaim(string filePath);
    ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults);
    Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct);
}
