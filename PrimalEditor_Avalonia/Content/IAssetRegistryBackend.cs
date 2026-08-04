using System;
using System.IO;

namespace PrimalEditor_Avalonia.Content;

public interface IAssetRegistryBackend
{
    void EnsureSchema();
    void UpsertAsset(Guid assetGuid, string relativePath, string assetType, AssetRecord record);
    void DeleteAsset(Guid assetGuid);
    void DeleteAssetByPath(string relativePath);
    StoredAsset? GetByPath(string relativePath);
    StoredAsset? GetByGuid(Guid assetGuid);
    void ReplaceDependencies(Guid owner, Guid[] dependencies);
    Guid[] GetDependencies(Guid owner);
    void UpsertImportJob(Guid assetGuid, string status, DateTime startedAt, DateTime finishedAt, string errorMessage);
    ImportJobRecord? GetImportJob(Guid assetGuid);

    // Default-returning reverse-dependency lookup. In-memory backends
    // that don't track reverse edges just return an empty array; the
    // SQLite backend overrides this with a real query.
    Guid[] GetReverseDependencies(Guid dependency) => System.Array.Empty<Guid>();

    // Build/Artifact APIs. Default interface methods so older in-memory
    // backends don't have to implement them — only SQLite does.
    void UpsertArtifact(string assetGuid, string platform, string configuration, string cookedPath, string artifactHash, DateTime producedAtUtc, string status)
        => throw new System.NotSupportedException("Artifacts are not supported by this backend.");

    void UpsertBuildManifest(string configKey, string manifestJson, DateTime producedAtUtc)
        => throw new System.NotSupportedException("Build manifests are not supported by this backend.");

    string? GetArtifactCookedPath(string assetGuid, string platform, string configuration) => null;
    string? GetBuildManifest(string configKey) => null;
}
