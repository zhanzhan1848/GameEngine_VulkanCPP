using System;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class SqliteGuidStoreTests
{
    [Fact]
    public void UpsertAndGetByPath_PersistsAcrossInstances()
    {
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        var path = Path.Combine(temp.Path, "Content", "A.material");

        var guid1 = Guid.NewGuid();
        using (var store = new SqliteGuidStore(dbPath))
        {
            store.SetGuid(path, guid1);
        }
        using (var store = new SqliteGuidStore(dbPath))
        {
            Assert.Equal(guid1, store.GetGuid(path));
        }
    }

    [Fact]
    public void RemovePath_DeletesEntry()
    {
        using var temp = new TemporaryProject();
        var dbPath = Path.Combine(temp.Path, ".Primal", "AssetIndex.sqlite");
        var path = Path.Combine(temp.Path, "Content", "A.material");
        using var store = new SqliteGuidStore(dbPath);
        store.SetGuid(path, Guid.NewGuid());
        store.Remove(path);
        Assert.Null(store.GetGuid(path));
    }
}
