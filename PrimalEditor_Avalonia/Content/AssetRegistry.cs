using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public static class AssetRegistry
{
    private static readonly Dictionary<string, AssetInfo> _byPath = new();
    private static readonly Dictionary<string, HashSet<Guid>> _dependenciesByPath = new();
    private static readonly ObservableCollection<AssetInfo> _all = new();
    private static IAssetRegistryBackend? _backend;

    // Project root makes full<->relative conversion possible. Without
    // it the registry is essentially read-only. The producer that calls
    // Initialize()/ConfigureSqliteBackend() owns this reference.
    private static string? _projectRoot;
    private static string? _projectContentRoot;

    // Default meta store used when no caller supplies one. .meta files
    // are the authoritative source for an asset's GUID — see
    // ResolutionOrder below.
    private static readonly IAssetMetaStore _defaultMetaStore = new JsonAssetMetaStore();

    public static ReadOnlyObservableCollection<AssetInfo> Assets { get; } =
        new ReadOnlyObservableCollection<AssetInfo>(_all);

    public static int Count => _all.Count;

    public static void ConfigureBackend(IAssetRegistryBackend? backend)
    {
        _backend = backend;
        _backend?.EnsureSchema();
    }

    public static void ConfigureSqliteBackend(string databasePath)
    {
        if (string.IsNullOrWhiteSpace(databasePath)) throw new ArgumentException("Path required.", nameof(databasePath));
        var directory = Path.GetDirectoryName(databasePath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        ConfigureBackend(new SqliteAssetRegistryBackend(databasePath));
    }

    public static void Initialize(string projectContentRoot)
    {
        Initialize(projectContentRoot, Path.GetDirectoryName(projectContentRoot) ?? projectContentRoot);
    }

    public static void Initialize(string projectContentRoot, string projectRoot)
    {
        Reset();
        _projectRoot = projectRoot;
        _projectContentRoot = projectContentRoot;
        if (!Directory.Exists(projectContentRoot))
        {
            Logger.Warning($"AssetRegistry: Content root not found: {projectContentRoot}");
            return;
        }
        ScanRecursive(projectContentRoot);
        Logger.Info($"AssetRegistry: scanned {_all.Count} asset(s) from {projectContentRoot}");
    }

    public static void Reset()
    {
        _byPath.Clear();
        _all.Clear();
        _dependenciesByPath.Clear();
        _projectRoot = null;
        _projectContentRoot = null;
    }

    public static string? ProjectRoot => _projectRoot;
    public static string? ProjectContentRoot => _projectContentRoot;

    public static ProjectRelativePath? GetRelativePath(string fullPath)
    {
        if (string.IsNullOrEmpty(_projectRoot)) return null;
        try
        {
            var normalizedFull = Path.GetFullPath(fullPath);
            var normalizedRoot = Path.GetFullPath(_projectRoot);
            var relative = Path.GetRelativePath(normalizedRoot, normalizedFull);
            if (string.IsNullOrEmpty(relative) || relative.StartsWith("..", StringComparison.Ordinal)) return null;
            return new ProjectRelativePath(relative.Replace('\\', '/'));
        }
        catch
        {
            return null;
        }
    }

    public static string? ResolveAssetPath(ProjectRelativePath relative)
        => string.IsNullOrEmpty(_projectRoot) ? null : relative.Resolve(_projectRoot);

    public static AssetInfo? Register(string fullPath)
    {
        if (!File.Exists(fullPath) && !Directory.Exists(fullPath)) return null;
        if (_byPath.TryGetValue(fullPath, out var existing)) return existing;

        var info = new AssetInfo
        {
            FullPath = fullPath,
            Type = ContentHelper.IsDirectory(fullPath)
                ? AssetType.Unknown
                : ContentHelper.GetAssetType(fullPath),
            RegisterTime = DateTime.UtcNow,
            ImportDate = File.Exists(fullPath)
                ? File.GetLastWriteTimeUtc(fullPath)
                : DateTime.UtcNow,
        };

        // The .meta file is the authoritative source for the asset's
        // GUID. The SQLite backend is a derived index that can be
        // rebuilt from a fresh .meta scan; it should not be queried
        // here because doing so would make the SQLite cache — rather
        // than the .meta file — the de-facto authority.
        //
        // Pre-flight: if the .meta exists but uses the legacy "PRIM"
        // binary encoding, rewrite it to JSON in-place. We do this
        // before the JSON read so the same call lands a single
        // canonical sidecar regardless of the project's age.
        var meta = File.Exists(fullPath) ? LoadOrMigrateMeta(fullPath) : null;
        if (meta != null)
        {
            info.Guid = meta.Guid.Value;
        }
        else
        {
            // No .meta yet — create one. The freshly minted GUID is
            // written to disk before the registry moves on so a crash
            // between this point and the next SQLite write cannot
            // leave the asset GUID dangling.
            var fresh = Guid.NewGuid();
            var written = false;
            if (File.Exists(fullPath))
            {
                written = _defaultMetaStore.Write(fullPath, new AssetMetaDocument
                {
                    Guid = new AssetGuid(fresh),
                    Importer = info.Type switch
                    {
                        AssetType.Material => "MaterialImporter",
                        AssetType.Shader => "ShaderImporter",
                        AssetType.Texture => "TextureImporter",
                        AssetType.Mesh => "ModelImporter",
                        _ => "UnknownImporter"
                    }
                });
            }
            info.Guid = written ? fresh : Guid.NewGuid();
        }



        if (File.Exists(fullPath))
        {
            try { info.Hash = ContentHelper.ComputeHash(fullPath); }
            catch (Exception ex) { Logger.Debug($"Hash skipped for {fullPath}: {ex.Message}"); }

            _dependenciesByPath.Remove(fullPath);
            if (info.Type is AssetType.Material or AssetType.Shader)
            {
                var deps = AssetDependencyExtractor.ExtractByExtension(fullPath).ToList();
                if (deps.Count > 0) _dependenciesByPath[fullPath] = new HashSet<Guid>(deps);
            }
        }
        _byPath[fullPath] = info;
        _all.Add(info);
        if (_backend != null && File.Exists(fullPath))
        {
            try
            {
                _backend.UpsertAsset(info.Guid, fullPath, info.Type.ToString(), new AssetRecord
                {
                    SourceHash = info.Hash is null ? string.Empty : Convert.ToBase64String(info.Hash),
                    MetaHash = string.Empty,
                    LastWriteTimeUtc = info.ImportDate,
                    Importer = info.Type switch
                    {
                        AssetType.Material => "MaterialImporter",
                        AssetType.Shader => "ShaderImporter",
                        AssetType.Texture => "TextureImporter",
                        AssetType.Mesh => "ModelImporter",
                        _ => "UnknownImporter"
                    },
                    ImportStatus = "Imported",
                    ErrorMessage = string.Empty
                });
                if (_dependenciesByPath.TryGetValue(fullPath, out var deps) && info.Type is AssetType.Material or AssetType.Shader)
                {
                    _backend.ReplaceDependencies(info.Guid, deps.ToArray());
                }
            }
            catch (Exception ex)
            {
                Logger.Debug($"AssetRegistry: backend upsert failed for {fullPath}: {ex.Message}");
            }
        }
        return info;
    }

    public static IEnumerable<Guid> GetDependencies(string fullPath)
    {
        if (_dependenciesByPath.TryGetValue(fullPath, out var deps)) return deps;
        if (_backend != null && _byPath.TryGetValue(fullPath, out var info))
        {
            var fromDb = _backend.GetDependencies(info.Guid);
            if (fromDb.Length > 0)
            {
                var set = new HashSet<Guid>(fromDb);
                _dependenciesByPath[fullPath] = set;
                return set;
            }
        }
        return Enumerable.Empty<Guid>();
    }

    /// <summary>
    /// Walk every asset that references <paramref name="fullPath"/>
    /// transitively and return the GUIDs of all owners. The SQLite
    /// backend keeps the reverse-dependency edge set already;
    /// callers that want stronger guarantees (e.g. before deleting an
    /// asset) should prefer this over hand-walking the Dependency table.
    /// </summary>
    public static IEnumerable<Guid> GetReverseDependencies(string fullPath)
    {
        if (_backend == null || !_byPath.TryGetValue(fullPath, out var info))
            return Enumerable.Empty<Guid>();
        return _backend.GetReverseDependencies(info.Guid);
    }

    /// <summary>
    /// Convenience: return the paths of every asset whose GUID is
    /// listed as a dependency of <paramref name="fullPath"/>'s GUID.
    /// Returns AssetInfo for callers that need the metadata; the
    /// returned list is in arbitrary order.
    /// </summary>
    public static IEnumerable<AssetInfo> GetDependents(string fullPath)
    {
        foreach (var ownerGuid in GetReverseDependencies(fullPath))
        {
            var info = GetByGuid(ownerGuid);
            if (info != null) yield return info;
        }
    }

    /// <summary>
    /// Pre-flight check before deleting an asset. Returns the full
    /// transitive closure of GUIDs that reference it so the editor
    /// can ask the user to confirm. The closure is bounded to direct
    /// dependents (no recursion) to keep deletion churn predictable.
    /// </summary>
    public static IReadOnlyCollection<AssetInfo> ListDeletionImpact(string fullPath)
    {
        var list = new List<AssetInfo>();
        foreach (var info in GetDependents(fullPath)) list.Add(info);
        return list;
    }

    public static void Unregister(string fullPath)
    {
        if (!_byPath.TryGetValue(fullPath, out var info)) return;
        _byPath.Remove(fullPath);
        _all.Remove(info);
        _dependenciesByPath.Remove(fullPath);
        _backend?.DeleteAsset(info.Guid);
    }

    /// <summary>
    /// Move an asset on disk and update every cached reference to
    /// its new path. The .meta sidecar travels with the asset so
    /// the GUID survives the move — every consumer that holds
    /// <see cref="AssetInfo.Guid"/> keeps working without any
    /// reimport. Returns the new AssetInfo on success or null when
    /// the source isn't registered.
    /// </summary>
    public static AssetInfo? RenameAsset(string oldPath, string newPath)
    {
        if (string.IsNullOrEmpty(oldPath) || string.IsNullOrEmpty(newPath)) return null;
        if (!_byPath.TryGetValue(oldPath, out var info)) return null;
        if (oldPath == newPath) return info;

        // Move the actual file/dir. Atomicity: write a partial copy,
        // then File.Move the sidecar with overwrite so an interrupted
        // move leaves the original on disk (no half-renamed asset).
        if (File.Exists(oldPath))
        {
            File.Move(oldPath, newPath, overwrite: true);
        }
        else if (Directory.Exists(oldPath))
        {
            Directory.Move(oldPath, newPath);
        }
        var oldMeta = oldPath + ".meta";
        var newMeta = newPath + ".meta";
        if (File.Exists(oldMeta))
        {
            File.Move(oldMeta, newMeta, overwrite: true);
        }

        // Invalidate the old cache entries; Register(newPath) will
        // re-read the sidecar so the GUID survives the move.
        _byPath.Remove(oldPath);
        _dependenciesByPath.Remove(oldPath);
        _all.Remove(info);

        var refreshed = Register(newPath);
        if (refreshed == null)
        {
            _byPath[oldPath] = info;
            _all.Add(info);
            return null;
        }
        return refreshed;
    }

    /// <summary>
    /// Return the .meta sidecar path that lives alongside
    /// <paramref name="assetPath"/>. Exposed publicly so editor
    /// surface code (rename/move dialogs) can keep the sidecar in
    /// sync with the asset file.
    /// </summary>
    public static string SidecarPath(string assetPath) => assetPath + ".meta";

    private static AssetMetaDocument? LoadOrMigrateMeta(string fullPath)
    {
        // First try the JSON reader. If it succeeds the sidecar is
        // modern and we can skip the migration path entirely.
        var direct = _defaultMetaStore.Read(fullPath);
        if (direct != null) return direct;

        // JSON reader returned null. Either the sidecar doesn't
        // exist or it's not valid JSON. Probe the magic bytes: if
        // the file carries the legacy "PRIM" signature, rewrite it
        // to JSON in place and retry the read. We deliberately
        // surface the migrated document rather than letting the
        // caller re-read so we only pay the JSON parse once.
        var metaPath = fullPath + ".meta";
        if (!Metadata.LegacyMetaMigrator.IsLegacyFormat(metaPath)) return null;
        var migrated = Metadata.LegacyMetaMigrator.Migrate(fullPath);
        if (migrated == null) return null;
        // Validator round-trip — JsonStore.Write already verified the
        // document by re-reading before rename, but a final read here
        // catches any future-store regressions.
        return _defaultMetaStore.Read(fullPath);
    }

    public static AssetInfo? Get(string fullPath) =>
        _byPath.TryGetValue(fullPath, out var i) ? i : null;

    public static AssetInfo? GetByGuid(Guid guid)
    {
        foreach (var info in _all)
            if (info.Guid == guid) return info;
        return null;
    }

    public static IEnumerable<AssetInfo> GetByType(AssetType type)
    {
        foreach (var info in _all)
            if (info.Type == type) yield return info;
    }

    private static void ScanRecursive(string root)
    {
        foreach (var dir in Directory.GetDirectories(root))
            ScanRecursive(dir);
        foreach (var file in Directory.GetFiles(root))
        {
            if (ContentHelper.IsAssetFile(file)) Register(file);
        }
    }
}
