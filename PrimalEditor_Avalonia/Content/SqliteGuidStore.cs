using System;
using System.IO;

namespace PrimalEditor_Avalonia.Content;

public sealed class SqliteGuidStore : IDisposable
{
    private readonly SqliteAssetIndex _index;

    public SqliteGuidStore(string databasePath)
    {
        if (string.IsNullOrWhiteSpace(databasePath)) throw new ArgumentException("Path required.", nameof(databasePath));
        var directory = Path.GetDirectoryName(databasePath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        _index = new SqliteAssetIndex(databasePath);
        _index.Open();
    }

    public void SetGuid(string relativePath, Guid guid) =>
        _index.SetAssetGuid(relativePath, guid);

    public Guid? GetGuid(string relativePath) =>
        _index.GetAssetGuid(relativePath);

    public void Remove(string relativePath) =>
        _index.RemoveAssetGuid(relativePath);

    public void Dispose() => _index.Dispose();
}
