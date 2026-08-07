using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import;
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
    public UserControl CreatePanel(IAssetImportSettings defaults)
    {
        var vm = new TextureImportPanelViewModel(defaults);
        var panel = new TextureImportPanel { DataContext = vm };
        return panel;
    }

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var sourceName = Path.GetFileNameWithoutExtension(request.SourcePath);
        var destTexPath = Path.Combine(request.DestinationDir, sourceName + EngineAssetExtension);
        Directory.CreateDirectory(request.DestinationDir);

        if (!NativeEngineBridge.IsLoaded)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = request.SourcePath,
                Type = AssetType.Texture,
                Success = false,
                ErrorMessage = "ContentTools not loaded; cannot cook texture.",
            };
        }

        TextureImportSettings settings = (TextureImportSettings)request.Settings;
        var cooked = await Task.Run(() => NativeEngineBridge.CookTexture(request.SourcePath, settings), ct);
        if (cooked == null)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destTexPath,
                Type = AssetType.Texture,
                Success = false,
                ErrorMessage = "ContentTools::CookTexture returned no result (corrupt source, unsupported format, or cook capacity exceeded).",
            };
        }

        try
        {
            TextureTexrWriter.Write(destTexPath, cooked);
        }
        catch (Exception ex)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = destTexPath,
                Type = AssetType.Texture,
                Success = false,
                ErrorMessage = $"TEXR write failed: {ex.Message}",
            };
        }

        AssetImportSettingsStore.Save<TextureImportSettings>(destTexPath, settings);

        return new ImportedAsset
        {
            SourcePath = request.SourcePath,
            DestinationPath = destTexPath,
            Type = AssetType.Texture,
            Success = true,
            Width = (int)cooked.Width,
            Height = (int)cooked.Height,
        };
    }

    public const string EngineAssetExtension = ".texture";
}
