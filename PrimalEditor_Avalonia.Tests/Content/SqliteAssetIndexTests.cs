using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class SqliteAssetIndexTests
{
    [Fact]
    public void Open_CreatesSchemaOnEmptyDatabase()
    {
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, "AssetIndex.sqlite");
        using var index = new SqliteAssetIndex(dbPath);
        index.Open();

        Assert.True(File.Exists(dbPath));
        var tables = index.ListTables();
        Assert.Contains("Assets", tables);
        Assert.Contains("Dependencies", tables);
        Assert.Contains("ImportJobs", tables);
    }

    [Fact]
    public void UpsertAsset_RoundTripsAndStores()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var guid = Guid.NewGuid();
        var path = Path.Combine(temp.Path, "Content", "Robot.material");
        index.UpsertAsset(guid, path, "Material", new AssetRecord
        {
            SourceHash = "abc123",
            MetaHash = "def456",
            LastWriteTimeUtc = DateTime.UtcNow,
            Importer = "MaterialImporter",
            ImportStatus = "Imported",
            ErrorMessage = string.Empty
        });

        var stored = index.GetByGuid(guid);
        Assert.NotNull(stored);
        Assert.Equal(path, stored!.RelativePath);
        Assert.Equal("Material", stored.AssetType);
        Assert.Equal("MaterialImporter", stored.Record.Importer);
        Assert.Equal("Imported", stored.Record.ImportStatus);
    }

    [Fact]
    public void UpsertAsset_DuplicatePathIsRejected()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var path = Path.Combine(temp.Path, "Content", "X.material");
        index.UpsertAsset(Guid.NewGuid(), path, "Material", new AssetRecord());
        Assert.Throws<InvalidOperationException>(() =>
            index.UpsertAsset(Guid.NewGuid(), path, "Material", new AssetRecord()));
    }

    [Fact]
    public void GetByPath_ReturnsCorrectAsset()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var guid = Guid.NewGuid();
        var path = Path.Combine(temp.Path, "Content", "Y.material");
        index.UpsertAsset(guid, path, "Material", new AssetRecord());

        var stored = index.GetByPath(path);
        Assert.NotNull(stored);
        Assert.Equal(guid, stored!.AssetGuid);
    }

    [Fact]
    public void ReplaceDependency_AtomicallyUpdatesEdges()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var material = Guid.NewGuid();
        var textureA = Guid.NewGuid();
        var textureB = Guid.NewGuid();
        index.UpsertAsset(material, "Content/M.material", "Material", new AssetRecord());
        index.UpsertAsset(textureA, "Content/A.png", "Texture", new AssetRecord());
        index.UpsertAsset(textureB, "Content/B.png", "Texture", new AssetRecord());

        index.ReplaceDependencies(material, new[] { textureA });
        Assert.Single(index.GetDependencies(material));
        index.ReplaceDependencies(material, new[] { textureA, textureB });
        var deps = index.GetDependencies(material).ToList();
        Assert.Equal(2, deps.Count);
        Assert.Contains(textureA, deps);
        Assert.Contains(textureB, deps);
    }

    [Fact]
    public void ImportJobs_UpsertRoundTrips()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var guid = Guid.NewGuid();
        var now = DateTime.UtcNow;
        index.UpsertImportJob(guid, "Imported", now, now.AddSeconds(1), string.Empty);
        var job = index.GetImportJob(guid);
        Assert.NotNull(job);
        Assert.Equal("Imported", job!.Status);
        Assert.Equal(string.Empty, job.ErrorMessage);
        Assert.True((now - job.StartedAt).Duration() < TimeSpan.FromSeconds(1));
    }

    [Fact]
    public void DeleteAsset_RemovesRow()
    {
        using var temp = new TemporaryProject();
        var index = new SqliteAssetIndex(Path.Combine(temp.Path, "AssetIndex.sqlite"));
        index.Open();
        var guid = Guid.NewGuid();
        index.UpsertAsset(guid, "Content/X.material", "Material", new AssetRecord());
        index.DeleteAsset(guid);
        Assert.Null(index.GetByPath("Content/X.material"));
        Assert.Null(index.GetByGuid(guid));
    }
}
