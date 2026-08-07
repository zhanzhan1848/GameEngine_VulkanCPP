using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Import.Settings;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class PrimitiveMeshImporterPlugin : IAssetImporterPlugin
{
    static PrimitiveMeshImporterPlugin()
    {
        AssetImporterRegistry.Register(new PrimitiveMeshImporterPlugin());
    }

    public string DisplayName => "Primitive Mesh Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Mesh };
    public Type SettingsType => typeof(PrimitiveInitInfo);

    public bool CanClaim(string filePath) => false;

    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new PrimitiveMeshImportPanelViewModel(defaults);

    public Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var info = (PrimitiveInitInfo)request.Settings;
        var destName = string.IsNullOrWhiteSpace(info.OutputName) ? "Primitive_Mesh" : info.OutputName;
        return Task.FromResult(new ImportedAsset
        {
            SourcePath = string.Empty,
            DestinationPath = Path.Combine(request.DestinationDir, destName + ".pm"),
            Type = AssetType.Mesh,
            Success = true,
        });
    }
}
