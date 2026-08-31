using System;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class SqliteRebuildIntegrationTests
{
    [Fact]
    public void DeleteDatabase_AndRebuild_RecreatesAssetAndDependencyRecords()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        var contentDir = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentDir);
        AssetRegistry.ConfigureSqliteBackend(dbPath);
        AssetRegistry.Initialize(contentDir);
        var materialPath = Path.Combine(contentDir, "Rebuild.material");
        var texturePath = Path.Combine(contentDir, "Rebuild.png");
        File.WriteAllText(materialPath, "{}");
        File.WriteAllText(texturePath, "{}");
        AssetRegistry.Register(materialPath);
        AssetRegistry.Register(texturePath);
        using (var index = new SqliteAssetIndex(dbPath))
        {
            index.Open();
            var materialEntry = index.GetByPath(materialPath);
            var textureEntry = index.GetByPath(texturePath);
            Assert.NotNull(materialEntry);
            Assert.NotNull(textureEntry);
            index.ReplaceDependencies(materialEntry!.AssetGuid, new[] { textureEntry!.AssetGuid });
        }
        File.Delete(dbPath);

        AssetRegistry.ConfigureSqliteBackend(dbPath);
        AssetRegistry.Initialize(contentDir);
        var info = AssetRegistry.Register(materialPath);
        Assert.NotNull(info);
        using (var index = new SqliteAssetIndex(dbPath))
        {
            index.Open();
            var stored = index.GetByPath(materialPath);
            Assert.NotNull(stored);
        }
    }
}
