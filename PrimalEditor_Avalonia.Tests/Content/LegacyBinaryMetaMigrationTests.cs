using System;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class LegacyBinaryMetaMigrationTests
{
    [Fact]
    public void LegacyBinaryMetaReader_AcceptsPrimV1Envelope()
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

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(AssetImportSettingsStore.Magic);
            bw.Write(AssetImportSettingsStore.CurrentVersion);
            settings.ToBinary(bw);
        }

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.True(result.Success);
        Assert.NotNull(result.Document);
        Assert.Equal(1, result.Document!.MetaVersion);
        Assert.NotEqual(Guid.Empty, result.Document.Guid.Value);
        Assert.Equal("TextureImporter", result.Document.Importer);
        Assert.NotNull(result.Document.Settings);
    }

    [Fact]
    public void LegacyBinaryMetaReader_GeneratesNonEmptyAssetGuid()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var settings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" }
        };

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(AssetImportSettingsStore.Magic);
            bw.Write(AssetImportSettingsStore.CurrentVersion);
            settings.ToBinary(bw);
        }

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.True(result.Success);
        Assert.NotEqual(Guid.Empty, result.Document!.Guid.Value);
    }

    [Fact]
    public void LegacyBinaryMetaReader_PreservesOriginalBytesOnFailure()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var originalContent = new byte[] { 0x50, 0x52, 0x49, 0x4D, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
        File.WriteAllBytes(assetPath + ".meta", originalContent);

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.False(result.Success);
        var currentContent = File.ReadAllBytes(assetPath + ".meta");
        Assert.Equal(originalContent, currentContent);
    }

    [Fact]
    public void LegacyBinaryMetaReader_RejectsInvalidMagic()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(0x12345678u);
            bw.Write(AssetImportSettingsStore.CurrentVersion);
        }

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.False(result.Success);
        Assert.Null(result.Document);
    }

    [Fact]
    public void LegacyBinaryMetaReader_RejectsUnknownVersion()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(AssetImportSettingsStore.Magic);
            bw.Write(999u);
        }

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.False(result.Success);
        Assert.Null(result.Document);
    }

    [Fact]
    public void LegacyBinaryMetaReader_RequiresExplicitImporterType()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var settings = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "test.png" }
        };

        using (var fs = File.Create(assetPath + ".meta"))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(AssetImportSettingsStore.Magic);
            bw.Write(AssetImportSettingsStore.CurrentVersion);
            settings.ToBinary(bw);
        }

        var reader = new LegacyBinaryMetaReader();

        Assert.Throws<ArgumentNullException>(() => reader.TryRead(assetPath + ".meta", null!));
    }

    [Fact]
    public void LegacyBinaryMetaReader_MigratesSettingsToJsonObject()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "texture.png");

        var settings = new TextureImportSettings
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
            settings.ToBinary(bw);
        }

        var reader = new LegacyBinaryMetaReader();
        var result = reader.TryRead(assetPath + ".meta", "TextureImporter");

        Assert.True(result.Success);
        Assert.NotNull(result.Document!.Settings);

        var jsonSettings = result.Document.Settings;
        Assert.True(jsonSettings.TryGetPropertyValue("Compress", out var compress));
        Assert.True(jsonSettings.TryGetPropertyValue("IsSRGB", out var isSRGB));
        Assert.True(jsonSettings.TryGetPropertyValue("MipLevels", out var mipLevels));
        Assert.True(jsonSettings.TryGetPropertyValue("IsNormalMap", out var isNormalMap));
    }
}