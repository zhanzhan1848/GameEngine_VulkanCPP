using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Microsoft.Data.Sqlite;

namespace PrimalEditor_Avalonia.Content;

public sealed class AssetRecord
{
    public string SourceHash { get; set; } = string.Empty;
    public string MetaHash { get; set; } = string.Empty;
    public DateTime LastWriteTimeUtc { get; set; }
    public string Importer { get; set; } = string.Empty;
    public string ImportStatus { get; set; } = "Pending";
    public string ErrorMessage { get; set; } = string.Empty;
}

public sealed class ImportJobRecord
{
    public string Status { get; set; } = string.Empty;
    public DateTime StartedAt { get; set; }
    public DateTime FinishedAt { get; set; }
    public string ErrorMessage { get; set; } = string.Empty;
}

public sealed class StoredAsset
{
    public Guid AssetGuid { get; set; }
    public string RelativePath { get; set; } = string.Empty;
    public string AssetType { get; set; } = string.Empty;
    public AssetRecord Record { get; set; } = new();
}

public sealed class SqliteAssetIndex : IDisposable
{
    private readonly string _connectionString;
    private SqliteConnection? _connection;

    public SqliteAssetIndex(string databasePath)
    {
        if (string.IsNullOrWhiteSpace(databasePath)) throw new ArgumentException("Path required.", nameof(databasePath));
        var directory = Path.GetDirectoryName(databasePath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = databasePath,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Cache = SqliteCacheMode.Shared
        }.ToString();
    }

    public void Open()
    {
        _connection = new SqliteConnection(_connectionString);
        _connection.Open();
        using var pragma = _connection.CreateCommand();
        pragma.CommandText = "PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON; PRAGMA synchronous = NORMAL;";
        pragma.ExecuteNonQuery();
        EnsureSchema();
        ApplyMigrations();
    }

    /// <summary>
    /// Latest schema version understood by this build. Bump when
    /// adding/removing columns; bump migration entries below at the
    /// same time so existing databases upgrade on next Open().
    /// </summary>
    public const int LatestSchemaVersion = 2;

    private void ApplyMigrations()
    {
        EnsureConnection();
        var current = TableExists("SchemaVersion") ? GetRecordedSchemaVersion() : 0;
        var pending = SqliteSchemaMigrations.All.Where(m => m.Version > current);
        foreach (var migration in pending)
        {
            using var cmd = _connection.CreateCommand();
            cmd.CommandText = migration.Sql;
            cmd.ExecuteNonQuery();
        }
        if (current != LatestSchemaVersion)
        {
            WriteSchemaVersion(LatestSchemaVersion, DateTime.UtcNow);
        }
    }

    private bool TableExists(string tableName)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=$name;";
        cmd.Parameters.AddWithValue("$name", tableName);
        var count = Convert.ToInt32(cmd.ExecuteScalar(), CultureInfo.InvariantCulture);
        return count > 0;
    }

    private int GetRecordedSchemaVersion()
    {
        EnsureConnection();
        using var cmd = _connection.CreateCommand();
        cmd.CommandText = "SELECT Version FROM SchemaVersion WHERE Id = 1;";
        var result = cmd.ExecuteScalar();
        if (result is null || result is DBNull) return 0;
        return Convert.ToInt32(result, CultureInfo.InvariantCulture);
    }

    private void WriteSchemaVersion(int version, DateTime migratedAtUtc)
    {
        EnsureConnection();
        using var cmd = _connection.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO SchemaVersion(Id, Version, MigratedAtUtc) VALUES(1, $v, $at)
            ON CONFLICT(Id) DO UPDATE SET Version = excluded.Version, MigratedAtUtc = excluded.MigratedAtUtc;
        ";
        cmd.Parameters.AddWithValue("$v", version);
        cmd.Parameters.AddWithValue("$at", migratedAtUtc.ToString("O", CultureInfo.InvariantCulture));
        cmd.ExecuteNonQuery();
    }

    private void EnsureSchema()
    {
        // v1 baseline (the four tables that existed before the
        // migration framework landed). The post-v1 tables (Artifacts,
        // BuildManifests, SchemaVersion) live in
        // SqliteSchemaMigrations so users iterating across builds can
        // upgrade an older DB without an EnsureSchema rewrite.
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            CREATE TABLE IF NOT EXISTS Assets(
                AssetGuid TEXT PRIMARY KEY,
                RelativePath TEXT NOT NULL,
                AssetType TEXT NOT NULL,
                SourceHash TEXT NOT NULL,
                MetaHash TEXT NOT NULL,
                LastWriteTimeUtc TEXT NOT NULL,
                Importer TEXT NOT NULL,
                ImportStatus TEXT NOT NULL,
                ErrorMessage TEXT NOT NULL,
                UNIQUE(RelativePath)
            );
            CREATE TABLE IF NOT EXISTS Dependencies(
                OwnerGuid TEXT NOT NULL,
                DependencyGuid TEXT NOT NULL,
                PRIMARY KEY (OwnerGuid, DependencyGuid)
            );
            CREATE TABLE IF NOT EXISTS ImportJobs(
                AssetGuid TEXT PRIMARY KEY,
                Status TEXT NOT NULL,
                StartedAt TEXT NOT NULL,
                FinishedAt TEXT NOT NULL,
                ErrorMessage TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS AssetGuids(
                RelativePath TEXT PRIMARY KEY,
                AssetGuid TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS IX_Assets_Path ON Assets(RelativePath);
            CREATE INDEX IF NOT EXISTS IX_Dependencies_Owner ON Dependencies(OwnerGuid);
            CREATE INDEX IF NOT EXISTS IX_ImportJobs_Status ON ImportJobs(Status);
        ";
        cmd.ExecuteNonQuery();
    }

    public IReadOnlyList<string> ListTables()
    {
        var result = new List<string>();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name";
        using var reader = cmd.ExecuteReader();
        while (reader.Read()) result.Add(reader.GetString(0));
        return result;
    }

    public void UpsertAsset(Guid assetGuid, string relativePath, string assetType, AssetRecord record)
    {
        EnsureConnection();
        if (assetGuid == Guid.Empty) throw new ArgumentException("Asset guid required.", nameof(assetGuid));
        if (string.IsNullOrWhiteSpace(relativePath)) throw new ArgumentException("Path required.", nameof(relativePath));
        if (record == null) throw new ArgumentNullException(nameof(record));

        using (var probe = _connection!.CreateCommand())
        {
            probe.CommandText = "SELECT COUNT(*) FROM Assets WHERE RelativePath = $path AND AssetGuid <> $guid;";
            probe.Parameters.AddWithValue("$path", relativePath);
            probe.Parameters.AddWithValue("$guid", assetGuid.ToString());
            var dupes = Convert.ToInt32(probe.ExecuteScalar(), CultureInfo.InvariantCulture);
            if (dupes > 0)
            {
                throw new InvalidOperationException($"A different asset is already registered at '{relativePath}'.");
            }
        }

        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO Assets(AssetGuid, RelativePath, AssetType, SourceHash, MetaHash, LastWriteTimeUtc, Importer, ImportStatus, ErrorMessage)
            VALUES($guid, $path, $type, $src, $meta, $lw, $imp, $status, $err)
            ON CONFLICT(AssetGuid) DO UPDATE SET
                RelativePath = excluded.RelativePath,
                AssetType = excluded.AssetType,
                SourceHash = excluded.SourceHash,
                MetaHash = excluded.MetaHash,
                LastWriteTimeUtc = excluded.LastWriteTimeUtc,
                Importer = excluded.Importer,
                ImportStatus = excluded.ImportStatus,
                ErrorMessage = excluded.ErrorMessage;
        ";
        cmd.Parameters.AddWithValue("$guid", assetGuid.ToString());
        cmd.Parameters.AddWithValue("$path", relativePath);
        cmd.Parameters.AddWithValue("$type", assetType ?? string.Empty);
        cmd.Parameters.AddWithValue("$src", record.SourceHash ?? string.Empty);
        cmd.Parameters.AddWithValue("$meta", record.MetaHash ?? string.Empty);
        cmd.Parameters.AddWithValue("$lw", record.LastWriteTimeUtc.ToString("O", CultureInfo.InvariantCulture));
        cmd.Parameters.AddWithValue("$imp", record.Importer ?? string.Empty);
        cmd.Parameters.AddWithValue("$status", record.ImportStatus ?? "Pending");
        cmd.Parameters.AddWithValue("$err", record.ErrorMessage ?? string.Empty);
        cmd.ExecuteNonQuery();
    }

    public void DeleteAsset(Guid assetGuid)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "DELETE FROM Assets WHERE AssetGuid = $guid; DELETE FROM Dependencies WHERE OwnerGuid = $guid OR DependencyGuid = $guid; DELETE FROM ImportJobs WHERE AssetGuid = $guid;";
        cmd.Parameters.AddWithValue("$guid", assetGuid.ToString());
        cmd.ExecuteNonQuery();
    }

    public void DeleteAssetByPath(string relativePath)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(relativePath)) return;
        var existing = GetByPath(relativePath);
        if (existing == null) return;
        DeleteAsset(existing.AssetGuid);
    }

    public StoredAsset? GetByGuid(Guid assetGuid)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"SELECT RelativePath, AssetType, SourceHash, MetaHash, LastWriteTimeUtc, Importer, ImportStatus, ErrorMessage FROM Assets WHERE AssetGuid = $guid;";
        cmd.Parameters.AddWithValue("$guid", assetGuid.ToString());
        using var reader = cmd.ExecuteReader();
        return reader.Read() ? ReadAsset(reader, assetGuid) : null;
    }

    public StoredAsset? GetByPath(string relativePath)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"SELECT AssetGuid, AssetType, SourceHash, MetaHash, LastWriteTimeUtc, Importer, ImportStatus, ErrorMessage FROM Assets WHERE RelativePath = $path;";
        cmd.Parameters.AddWithValue("$path", relativePath);
        using var reader = cmd.ExecuteReader();
        if (!reader.Read()) return null;
        var guid = Guid.Parse(reader.GetString(0));
        return ReadAsset(reader, guid);
    }

    public void ReplaceDependencies(Guid owner, IEnumerable<Guid> dependencies)
    {
        EnsureConnection();
        if (owner == Guid.Empty) throw new ArgumentException("Owner required.", nameof(owner));
        using (var tx = _connection!.BeginTransaction())
        {
            using (var del = _connection.CreateCommand())
            {
                del.CommandText = "DELETE FROM Dependencies WHERE OwnerGuid = $owner;";
                del.Parameters.AddWithValue("$owner", owner.ToString());
                del.Transaction = tx;
                del.ExecuteNonQuery();
            }
            foreach (var dep in dependencies ?? Array.Empty<Guid>())
            {
                if (dep == Guid.Empty) continue;
                using var ins = _connection.CreateCommand();
                ins.CommandText = "INSERT INTO Dependencies(OwnerGuid, DependencyGuid) VALUES($owner, $dep);";
                ins.Parameters.AddWithValue("$owner", owner.ToString());
                ins.Parameters.AddWithValue("$dep", dep.ToString());
                ins.Transaction = tx;
                ins.ExecuteNonQuery();
            }
            tx.Commit();
        }
    }

    public IEnumerable<Guid> GetDependencies(Guid owner)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT DependencyGuid FROM Dependencies WHERE OwnerGuid = $owner;";
        cmd.Parameters.AddWithValue("$owner", owner.ToString());
        using var reader = cmd.ExecuteReader();
        while (reader.Read()) yield return Guid.Parse(reader.GetString(0));
    }

    public IEnumerable<Guid> GetReverseDependencies(Guid dependency)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT OwnerGuid FROM Dependencies WHERE DependencyGuid = $dep;";
        cmd.Parameters.AddWithValue("$dep", dependency.ToString());
        using var reader = cmd.ExecuteReader();
        while (reader.Read()) yield return Guid.Parse(reader.GetString(0));
    }

    public void UpsertImportJob(Guid assetGuid, string status, DateTime startedAt, DateTime finishedAt, string errorMessage)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO ImportJobs(AssetGuid, Status, StartedAt, FinishedAt, ErrorMessage)
            VALUES($guid, $status, $started, $finished, $err)
            ON CONFLICT(AssetGuid) DO UPDATE SET
                Status = excluded.Status,
                StartedAt = excluded.StartedAt,
                FinishedAt = excluded.FinishedAt,
                ErrorMessage = excluded.ErrorMessage;
        ";
        cmd.Parameters.AddWithValue("$guid", assetGuid.ToString());
        cmd.Parameters.AddWithValue("$status", status ?? string.Empty);
        cmd.Parameters.AddWithValue("$started", startedAt.ToString("O", CultureInfo.InvariantCulture));
        cmd.Parameters.AddWithValue("$finished", finishedAt.ToString("O", CultureInfo.InvariantCulture));
        cmd.Parameters.AddWithValue("$err", errorMessage ?? string.Empty);
        cmd.ExecuteNonQuery();
    }

    public ImportJobRecord? GetImportJob(Guid assetGuid)
    {
        EnsureConnection();
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT Status, StartedAt, FinishedAt, ErrorMessage FROM ImportJobs WHERE AssetGuid = $guid;";
        cmd.Parameters.AddWithValue("$guid", assetGuid.ToString());
        using var reader = cmd.ExecuteReader();
        if (!reader.Read()) return null;
        return new ImportJobRecord
        {
            Status = reader.GetString(0),
            StartedAt = DateTime.Parse(reader.GetString(1), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
            FinishedAt = DateTime.Parse(reader.GetString(2), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
            ErrorMessage = reader.GetString(3)
        };
    }

    public void SetAssetGuid(string relativePath, Guid guid)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(relativePath)) throw new ArgumentException("Path required.", nameof(relativePath));
        if (guid == Guid.Empty) throw new ArgumentException("Guid required.", nameof(guid));
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO AssetGuids(RelativePath, AssetGuid) VALUES($path, $guid)
            ON CONFLICT(RelativePath) DO UPDATE SET AssetGuid = excluded.AssetGuid;";
        cmd.Parameters.AddWithValue("$path", relativePath);
        cmd.Parameters.AddWithValue("$guid", guid.ToString());
        cmd.ExecuteNonQuery();
    }

    public Guid? GetAssetGuid(string relativePath)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(relativePath)) return null;
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT AssetGuid FROM AssetGuids WHERE RelativePath = $path;";
        cmd.Parameters.AddWithValue("$path", relativePath);
        var result = cmd.ExecuteScalar();
        if (result is null || result is DBNull) return null;
        return Guid.Parse((string)result, CultureInfo.InvariantCulture);
    }

    public void RemoveAssetGuid(string relativePath)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(relativePath)) return;
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "DELETE FROM AssetGuids WHERE RelativePath = $path;";
        cmd.Parameters.AddWithValue("$path", relativePath);
        cmd.ExecuteNonQuery();
    }

    public void UpsertArtifact(string assetGuid, string platform, string configuration, string cookedPath, string artifactHash, DateTime producedAtUtc, string status)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(assetGuid)) throw new ArgumentException("Asset guid required.", nameof(assetGuid));
        if (string.IsNullOrWhiteSpace(platform)) throw new ArgumentException("Platform required.", nameof(platform));
        if (string.IsNullOrWhiteSpace(configuration)) throw new ArgumentException("Configuration required.", nameof(configuration));
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO Artifacts(AssetGuid, Platform, Configuration, CookedPath, ArtifactHash, ProducedAtUtc, Status)
            VALUES($guid, $plat, $cfg, $path, $hash, $at, $status)
            ON CONFLICT(AssetGuid, Platform, Configuration) DO UPDATE SET
                CookedPath = excluded.CookedPath,
                ArtifactHash = excluded.ArtifactHash,
                ProducedAtUtc = excluded.ProducedAtUtc,
                Status = excluded.Status;
        ";
        cmd.Parameters.AddWithValue("$guid", assetGuid);
        cmd.Parameters.AddWithValue("$plat", platform);
        cmd.Parameters.AddWithValue("$cfg", configuration);
        cmd.Parameters.AddWithValue("$path", cookedPath ?? string.Empty);
        cmd.Parameters.AddWithValue("$hash", artifactHash ?? string.Empty);
        cmd.Parameters.AddWithValue("$at", producedAtUtc.ToString("O", CultureInfo.InvariantCulture));
        cmd.Parameters.AddWithValue("$status", status ?? "Unknown");
        cmd.ExecuteNonQuery();
    }

    public void DeleteArtifactsForAsset(string assetGuid)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(assetGuid)) return;
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "DELETE FROM Artifacts WHERE AssetGuid = $guid;";
        cmd.Parameters.AddWithValue("$guid", assetGuid);
        cmd.ExecuteNonQuery();
    }

    public string? GetArtifactCookedPath(string assetGuid, string platform, string configuration)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(assetGuid)) return null;
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT CookedPath FROM Artifacts WHERE AssetGuid = $guid AND Platform = $plat AND Configuration = $cfg;";
        cmd.Parameters.AddWithValue("$guid", assetGuid);
        cmd.Parameters.AddWithValue("$plat", platform);
        cmd.Parameters.AddWithValue("$cfg", configuration);
        var result = cmd.ExecuteScalar();
        if (result is null || result is DBNull) return null;
        return (string)result;
    }

    public void UpsertBuildManifest(string configKey, string manifestJson, DateTime producedAtUtc)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(configKey)) throw new ArgumentException("ConfigKey required.", nameof(configKey));
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = @"
            INSERT INTO BuildManifests(ConfigKey, ManifestJson, ProducedAtUtc)
            VALUES($key, $json, $at)
            ON CONFLICT(ConfigKey) DO UPDATE SET
                ManifestJson = excluded.ManifestJson,
                ProducedAtUtc = excluded.ProducedAtUtc;
        ";
        cmd.Parameters.AddWithValue("$key", configKey);
        cmd.Parameters.AddWithValue("$json", manifestJson ?? string.Empty);
        cmd.Parameters.AddWithValue("$at", producedAtUtc.ToString("O", CultureInfo.InvariantCulture));
        cmd.ExecuteNonQuery();
    }

    public string? GetBuildManifest(string configKey)
    {
        EnsureConnection();
        if (string.IsNullOrWhiteSpace(configKey)) return null;
        using var cmd = _connection!.CreateCommand();
        cmd.CommandText = "SELECT ManifestJson FROM BuildManifests WHERE ConfigKey = $key;";
        cmd.Parameters.AddWithValue("$key", configKey);
        var result = cmd.ExecuteScalar();
        if (result is null || result is DBNull) return null;
        return (string)result;
    }

    private void EnsureConnection()
    {
        if (_connection == null) throw new InvalidOperationException("SqliteAssetIndex.Open must be called first.");
    }

    private static StoredAsset ReadAsset(SqliteDataReader reader, Guid guid)
    {
        var record = new AssetRecord
        {
            SourceHash = reader.GetString(2),
            MetaHash = reader.GetString(3),
            LastWriteTimeUtc = DateTime.Parse(reader.GetString(4), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind),
            Importer = reader.GetString(5),
            ImportStatus = reader.GetString(6),
            ErrorMessage = reader.GetString(7)
        };
        var path = reader.GetString(0);
        var type = reader.GetString(1);
        return new StoredAsset
        {
            AssetGuid = guid,
            RelativePath = path,
            AssetType = type,
            Record = record
        };
    }

    public void Dispose()
    {
        _connection?.Dispose();
        _connection = null;
        SqliteConnection.ClearAllPools();
    }
}
