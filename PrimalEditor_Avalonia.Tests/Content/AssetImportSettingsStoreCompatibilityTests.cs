using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class AssetImportSettingsStoreCompatibilityTests
{
    [Fact]
    public void Save_WritesJsonMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");
        var metaPath = assetPath + ".meta";

        var settings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" },
            Dimension = TextureDimension.Texture2D,
            MipLevels = 5,
            Compress = true,
            IsSRGB = false
        };

        var result = AssetImportSettingsStore.Save(assetPath, settings);

        Assert.True(result);
        Assert.True(File.Exists(metaPath));

        var content = File.ReadAllText(metaPath);
        Assert.Contains("\"MetaVersion\"", content);
        Assert.Contains("\"Guid\"", content);
        Assert.Contains("\"Importer\"", content);
        Assert.Contains("\"Settings\"", content);
    }

    [Fact]
    public void Load_ReadsLegacyBinaryMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var originalSettings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" },
            Dimension = TextureDimension.TextureCube,
            MipLevels = 7,
            AlphaThreshold = 0.75f,
            PreferBC7 = false,
            OutputFormat = TextureFormat.BC3_UNORM,
            Compress = false,
            IsSRGB = true,
            CompressionMode = TextureCompressionMode.Quality,
            ArraySize = 6,
            Depth = 1,
            IsNormalMap = true
        };

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(AssetImportSettingsStore.Magic);
            bw.Write(AssetImportSettingsStore.CurrentVersion);
            originalSettings.ToBinary(bw);
        }

        var loadedSettings = AssetImportSettingsStore.Load<TextureImportSettings>(assetPath);

        Assert.NotNull(loadedSettings);
        Assert.Equal(originalSettings.Dimension, loadedSettings!.Dimension);
        Assert.Equal(originalSettings.MipLevels, loadedSettings.MipLevels);
        Assert.Equal(originalSettings.AlphaThreshold, loadedSettings.AlphaThreshold);
        Assert.Equal(originalSettings.PreferBC7, loadedSettings.PreferBC7);
        Assert.Equal(originalSettings.OutputFormat, loadedSettings.OutputFormat);
        Assert.Equal(originalSettings.Compress, loadedSettings.Compress);
        Assert.Equal(originalSettings.IsSRGB, loadedSettings.IsSRGB);
        Assert.Equal(originalSettings.CompressionMode, loadedSettings.CompressionMode);
        Assert.Equal(originalSettings.ArraySize, loadedSettings.ArraySize);
        Assert.Equal(originalSettings.Depth, loadedSettings.Depth);
        Assert.Equal(originalSettings.IsNormalMap, loadedSettings.IsNormalMap);
    }

    [Fact]
    public void Load_ReadsJsonMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var settings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" },
            Dimension = TextureDimension.Texture2D,
            MipLevels = 5,
            Compress = true,
            IsSRGB = false
        };

        var saveResult = AssetImportSettingsStore.Save(assetPath, settings);
        Assert.True(saveResult);

        var loadedSettings = AssetImportSettingsStore.Load<TextureImportSettings>(assetPath);

        Assert.NotNull(loadedSettings);
        Assert.Equal(TextureDimension.Texture2D, loadedSettings!.Dimension);
        Assert.Equal(5, loadedSettings.MipLevels);
        Assert.True(loadedSettings.Compress);
        Assert.False(loadedSettings.IsSRGB);
    }

    [Fact]
    public void Save_WritesAtomicWithPartial()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");
        var metaPath = assetPath + ".meta";
        var partialPath = metaPath + ".partial";

        var settings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" }
        };

        var result = AssetImportSettingsStore.Save(assetPath, settings);

        Assert.True(result);
        Assert.False(File.Exists(partialPath));
        Assert.True(File.Exists(metaPath));
    }

    [Fact]
    public void Load_ReturnsDefaultForMissingMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "nonexistent.png");

        var settings = AssetImportSettingsStore.Load<TextureImportSettings>(assetPath);

        Assert.NotNull(settings);
        Assert.Equal(TextureDimension.Texture2D, settings!.Dimension);
        Assert.True(settings.IsSRGB);
    }

    [Fact]
    public void ExistingCallSites_KeepCompiling()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "test.png");

        var settings = new TextureImportSettings();

        var saveResult = AssetImportSettingsStore.Save(assetPath, settings);
        var loadedSettings = AssetImportSettingsStore.Load<TextureImportSettings>(assetPath);

        Assert.NotNull(loadedSettings);
    }
}