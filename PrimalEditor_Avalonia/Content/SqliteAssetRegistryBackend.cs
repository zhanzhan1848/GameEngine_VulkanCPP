using System;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Content;

public sealed class SqliteAssetRegistryBackend : IAssetRegistryBackend, System.IDisposable
{
    private readonly SqliteAssetIndex _index;

    public SqliteAssetRegistryBackend(string databasePath)
    {
        if (string.IsNullOrWhiteSpace(databasePath)) throw new ArgumentException("Path required.", nameof(databasePath));
        var directory = Path.GetDirectoryName(databasePath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        _index = new SqliteAssetIndex(databasePath);
        _index.Open();
    }

    public void EnsureSchema() { /* schema ensured via Open() */ }

    public void UpsertAsset(Guid assetGuid, string relativePath, string assetType, AssetRecord record)
    {
        EnsureValidPath(relativePath);
        var existing = _index.GetByPath(relativePath);
        if (existing == null)
        {
            _index.UpsertAsset(assetGuid, relativePath, assetType, record ?? new AssetRecord());
        }
        else if (existing.AssetGuid != assetGuid)
        {
            _index.UpsertAsset(existing.AssetGuid, relativePath, assetType, record ?? new AssetRecord());
        }
        else
        {
            _index.UpsertAsset(assetGuid, relativePath, assetType, record ?? new AssetRecord());
        }
    }

    public void DeleteAsset(Guid assetGuid) => _index.DeleteAsset(assetGuid);

    public void DeleteAssetByPath(string relativePath) => _index.DeleteAssetByPath(relativePath);

    public StoredAsset? GetByPath(string relativePath) =>
        _index.GetByPath(relativePath);

    public StoredAsset? GetByGuid(Guid assetGuid) =>
        _index.GetByGuid(assetGuid);

    public void ReplaceDependencies(Guid owner, Guid[] dependencies) =>
        _index.ReplaceDependencies(owner, dependencies ?? Array.Empty<Guid>());

    public Guid[] GetDependencies(Guid owner) =>
        _index.GetDependencies(owner).ToArray();

    public Guid[] GetReverseDependencies(Guid dependency) =>
        _index.GetReverseDependencies(dependency).ToArray();

    public void UpsertImportJob(Guid assetGuid, string status, DateTime startedAt, DateTime finishedAt, string errorMessage) =>
        _index.UpsertImportJob(assetGuid, status, startedAt, finishedAt, errorMessage);

    public ImportJobRecord? GetImportJob(Guid assetGuid) =>
        _index.GetImportJob(assetGuid);

    public void UpsertArtifact(string assetGuid, string platform, string configuration, string cookedPath, string artifactHash, DateTime producedAtUtc, string status) =>
        _index.UpsertArtifact(assetGuid, platform, configuration, cookedPath, artifactHash, producedAtUtc, status);

    public void UpsertBuildManifest(string configKey, string manifestJson, DateTime producedAtUtc) =>
        _index.UpsertBuildManifest(configKey, manifestJson, producedAtUtc);

    public string? GetArtifactCookedPath(string assetGuid, string platform, string configuration) =>
        _index.GetArtifactCookedPath(assetGuid, platform, configuration);

    public string? GetBuildManifest(string configKey) =>
        _index.GetBuildManifest(configKey);

    private static void EnsureValidPath(string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath)) throw new ArgumentException("Path required.", nameof(relativePath));
    }

    public void Dispose() => _index.Dispose();
}

internal static class SqliteAssetIndexExtensions
{
    public static void Let<T>(this T? value, Action<T> action) where T : class
    {
        if (value != null) action(value);
    }
}
