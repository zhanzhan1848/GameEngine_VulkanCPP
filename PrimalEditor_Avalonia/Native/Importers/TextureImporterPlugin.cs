using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import.Settings;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class TextureImporterPlugin : IAssetImporterPlugin
{
    private static readonly HashSet<string> _exts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".ktx", ".ktx2"
    };

    static TextureImporterPlugin()
    {
        AssetImporterRegistry.Register(new TextureImporterPlugin());
    }

    public string DisplayName => "Texture Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Texture };
    public Type SettingsType => typeof(TextureImportSettings);
    public bool CanClaim(string filePath) => _exts.Contains(Path.GetExtension(filePath));
    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new TextureImportPanelViewModel(defaults);

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var destPath = Path.Combine(request.DestinationDir, Path.GetFileName(request.SourcePath));
        Directory.CreateDirectory(request.DestinationDir);

        try { File.Copy(request.SourcePath, destPath, overwrite: true); }
        catch (Exception ex)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destPath,
                Type = AssetType.Texture,
                Success = false,
                ErrorMessage = ex.Message,
            };
        }

        AssetImportSettingsStore.Save<TextureImportSettings>(destPath, (TextureImportSettings)request.Settings);

        if (!NativeEngineBridge.IsLoaded)
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Texture, Success = true };

        var result = await Task.Run(() => NativeEngineBridge.ImportTexture(destPath), ct);
        if (!result.HasValue || result.Value.Success == 0)
        {
            var err = result?.ErrorMessage;
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destPath,
                Type = AssetType.Texture,
                Success = false,
                ErrorMessage = string.IsNullOrEmpty(err) ? "Texture import returned no result" : err,
            };
        }

        return new ImportedAsset
        {
            SourcePath = request.SourcePath,
            DestinationPath = destPath,
            Type = AssetType.Texture,
            Success = true,
            NativeHandle = result.Value.Handle,
            Width = (int)result.Value.Width,
            Height = (int)result.Value.Height,
        };
    }
}
