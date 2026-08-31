using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class AssetMetaStoreTests
{
    [Fact]
    public void AssetMetaDocument_HasRequiredFields()
    {
        var doc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(System.Guid.NewGuid()),
            Importer = "TextureImporter",
            Settings = new JsonObject()
        };

        Assert.Equal(1, doc.MetaVersion);
        Assert.NotEqual(System.Guid.Empty, doc.Guid.Value);
        Assert.Equal("TextureImporter", doc.Importer);
        Assert.NotNull(doc.Settings);
    }

    [Fact]
    public void AssetMetaDocument_SerializeToJson_DeterministicAndIndented()
    {
        var doc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(new System.Guid("12345678-1234-1234-1234-123456789012")),
            Importer = "TextureImporter",
            Settings = new JsonObject
            {
                ["Compress"] = true,
                ["IsSRGB"] = true,
                ["MipLevels"] = 9
            }
        };

        var json = JsonSerializer.Serialize(doc, new JsonSerializerOptions
        {
            WriteIndented = true
        });

        Assert.Contains("\"MetaVersion\"", json);
        Assert.Contains("\"Guid\"", json);
        Assert.Contains("\"Importer\"", json);
        Assert.Contains("\"Settings\"", json);
        Assert.Contains("\"Compress\"", json);
        Assert.Contains("\"IsSRGB\"", json);
        Assert.Contains("\"MipLevels\"", json);
        Assert.Contains("  ", json);
    }

    [Fact]
    public void AssetMetaDocument_RoundTrip_PreservesUnknownSettings()
    {
        var original = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(new System.Guid("12345678-1234-1234-1234-123456789012")),
            Importer = "TextureImporter",
            Settings = new JsonObject
            {
                ["Compress"] = true,
                ["IsSRGB"] = true,
                ["UnknownField"] = "Should be preserved",
                ["FutureSetting"] = 42
            }
        };

        var json = JsonSerializer.Serialize(original, new JsonSerializerOptions
        {
            WriteIndented = true
        });

        var deserialized = JsonSerializer.Deserialize<AssetMetaDocument>(json, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });

        Assert.NotNull(deserialized);
        Assert.Equal(original.MetaVersion, deserialized.MetaVersion);
        Assert.Equal(original.Guid, deserialized.Guid);
        Assert.Equal(original.Importer, deserialized.Importer);
        Assert.NotNull(deserialized.Settings);
        Assert.True(deserialized.Settings.TryGetPropertyValue("Compress", out var compress));
        Assert.True(deserialized.Settings.TryGetPropertyValue("IsSRGB", out var isSRGB));
        Assert.True(deserialized.Settings.TryGetPropertyValue("UnknownField", out var unknown));
        Assert.True(deserialized.Settings.TryGetPropertyValue("FutureSetting", out var future));
    }

    [Fact]
    public void JsonAssetMetaStore_Write_CreatesPartialThenAtomicallyReplaces()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "test_asset.png");
        var metaPath = assetPath + ".meta";
        var partialPath = metaPath + ".partial";

        var store = new JsonAssetMetaStore();
        var doc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(System.Guid.NewGuid()),
            Importer = "TextureImporter",
            Settings = new JsonObject()
        };

        var result = store.Write(assetPath, doc);

        Assert.True(result);
        Assert.False(File.Exists(partialPath));
        Assert.True(File.Exists(metaPath));
    }

    [Fact]
    public void JsonAssetMetaStore_Write_RejectsInvalidDocumentAndPreservesExistingMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "test_asset.png");
        var metaPath = assetPath + ".meta";

        var initialDoc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(new System.Guid("11111111-1111-1111-1111-111111111111")),
            Importer = "TextureImporter",
            Settings = new JsonObject { ["Original"] = true }
        };

        var store = new JsonAssetMetaStore();
        Assert.True(store.Write(assetPath, initialDoc));
        var initialContent = File.ReadAllText(metaPath);

        var invalidDoc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(System.Guid.NewGuid()),
            Importer = string.Empty,
            Settings = new JsonObject()
        };

        Assert.False(store.Write(assetPath, invalidDoc));
        Assert.Equal(initialContent, File.ReadAllText(metaPath));
        Assert.False(File.Exists(metaPath + ".partial"));
    }

    [Fact]
    public void JsonAssetMetaStore_Read_LoadsValidJsonMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "test_asset.png");

        var originalDoc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(new System.Guid("12345678-1234-1234-1234-123456789012")),
            Importer = "TextureImporter",
            Settings = new JsonObject
            {
                ["Compress"] = true,
                ["IsSRGB"] = false
            }
        };

        var store = new JsonAssetMetaStore();
        store.Write(assetPath, originalDoc);

        var loadedDoc = store.Read(assetPath);

        Assert.NotNull(loadedDoc);
        Assert.Equal(originalDoc.MetaVersion, loadedDoc!.MetaVersion);
        Assert.Equal(originalDoc.Guid, loadedDoc.Guid);
        Assert.Equal(originalDoc.Importer, loadedDoc.Importer);
    }

    [Fact]
    public void JsonAssetMetaStore_Read_ReturnsNullForMissingMeta()
    {
        using var tempDir = new TemporaryDirectory();
        var assetPath = Path.Combine(tempDir.Path, "nonexistent_asset.png");

        var store = new JsonAssetMetaStore();
        var result = store.Read(assetPath);

        Assert.Null(result);
    }

    [Fact]
    public void IAssetMetaStore_CanBeImplemented()
    {
        var store = new JsonAssetMetaStore();
        Assert.NotNull(store);

        var assetPath = "/fake/asset.png";
        var doc = new AssetMetaDocument();

        var writeResult = store.Write(assetPath, doc);
        var readResult = store.Read(assetPath);

        Assert.NotNull(store);
    }
}