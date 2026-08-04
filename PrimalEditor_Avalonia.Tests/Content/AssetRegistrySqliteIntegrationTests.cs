using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class AssetRegistrySqliteIntegrationTests
{
    [Fact]
    public void Register_PersistsAssetToSqlite()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        ConfigureBackend(dbPath);
        AssetRegistry.Initialize(temp.ContentPath);
        var materialPath = Path.Combine(temp.ContentPath, "Robot.material");
        File.WriteAllText(materialPath, "{}");

        AssetRegistry.Register(materialPath);
        using var index = new SqliteAssetIndex(dbPath);
        index.Open();
        var stored = index.GetByPath(materialPath);
        Assert.NotNull(stored);
        Assert.Equal(AssetType.Material, ResolveType(materialPath));
    }

    [Fact]
    public void Unregister_RemovesAssetFromSqlite()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        ConfigureBackend(dbPath);
        AssetRegistry.Initialize(temp.ContentPath);
        var path = Path.Combine(temp.ContentPath, "Z.material");
        File.WriteAllText(path, "{}");
        var info = AssetRegistry.Register(path);
        Assert.NotNull(info);
        var guid = info!.Guid;
        AssetRegistry.Unregister(path);
        using var index = new SqliteAssetIndex(dbPath);
        index.Open();
        Assert.Null(index.GetByPath(path));
        Assert.Null(index.GetByGuid(guid));
    }

    [Fact]
    public void GetDependencies_RoundTripsFromSqlite()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        ConfigureBackend(dbPath);
        AssetRegistry.Initialize(temp.ContentPath);
        var materialPath = Path.Combine(temp.ContentPath, "M.material");
        var albedoPath = Path.Combine(temp.ContentPath, "A.png");
        File.WriteAllText(materialPath, "{}");
        File.WriteAllText(albedoPath, "{}");
        AssetRegistry.Register(materialPath);
        AssetRegistry.Register(albedoPath);
        using var index = new SqliteAssetIndex(dbPath);
        index.Open();
        var material = index.GetByPath(materialPath);
        var albedo = index.GetByPath(albedoPath);
        Assert.NotNull(material);
        Assert.NotNull(albedo);
        index.ReplaceDependencies(material!.AssetGuid, new[] { albedo!.AssetGuid });

        var deps = AssetRegistry.GetDependencies(materialPath).ToList();
        Assert.Single(deps);
        Assert.Equal(albedo.AssetGuid, deps[0]);
    }

    [Fact]
    public void RebuildFromDeletedDatabase_RecreatesSchema()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        ConfigureBackend(dbPath);
        AssetRegistry.Initialize(temp.ContentPath);
        File.Delete(dbPath);
        ConfigureBackend(dbPath);
        AssetRegistry.Initialize(temp.ContentPath);
        var path = Path.Combine(temp.ContentPath, "Restored.material");
        File.WriteAllText(path, "{}");
        var info = AssetRegistry.Register(path);
        Assert.NotNull(info);
    }

    private static void ConfigureBackend(string databasePath)
    {
        AssetRegistry.ConfigureBackend(new SqliteAssetRegistryBackend(databasePath));
    }

    private static AssetType ResolveType(string path) =>
        ContentHelper.GetAssetType(path);
}
