using System.Collections.Generic;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Ordered list of SQLite schema migrations. Each entry's
/// <see cref="Version"/> must match a milestone in
/// <see cref="SqliteAssetIndex.LatestSchemaVersion"/>; SQL runs when
/// the on-disk schema is older than the entry's version.
///
/// Add migrations by appending a new entry — never edit an existing
/// one's <see cref="Sql"/>; the migration framework trusts the order
/// it's recorded in.
/// </summary>
public static class SqliteSchemaMigrations
{
    public sealed class Migration
    {
        public int Version { get; init; }
        public string Description { get; init; } = string.Empty;
        public string Sql { get; init; } = string.Empty;
    }

    public static IReadOnlyList<Migration> All { get; } = new[]
    {
        // v1 is the baseline created by EnsureSchema (legacy tables
        // exist before the migration framework did). No SQL needed —
        // the framework writes the row on first Open.
        new Migration { Version = 1, Description = "baseline (legacy tables pre-framework)", Sql = "" },

        // v2 introduces Cooked Artifact rows, BuildManifest rows, and
        // the SchemaVersion book-keeping table itself. Idempotent:
        // CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS let
        // the migration run against a fresh DB too.
        new Migration
        {
            Version = 2,
            Description = "add Artifacts, BuildManifests, SchemaVersion",
            Sql = @"
                CREATE TABLE IF NOT EXISTS Artifacts(
                    AssetGuid TEXT NOT NULL,
                    Platform TEXT NOT NULL,
                    Configuration TEXT NOT NULL,
                    CookedPath TEXT NOT NULL,
                    ArtifactHash TEXT NOT NULL,
                    ProducedAtUtc TEXT NOT NULL,
                    Status TEXT NOT NULL,
                    PRIMARY KEY (AssetGuid, Platform, Configuration)
                );
                CREATE TABLE IF NOT EXISTS BuildManifests(
                    ConfigKey TEXT PRIMARY KEY,
                    ManifestJson TEXT NOT NULL,
                    ProducedAtUtc TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS SchemaVersion(
                    Id INTEGER PRIMARY KEY CHECK (Id = 1),
                    Version INTEGER NOT NULL,
                    MigratedAtUtc TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS IX_Artifacts_PlatformConfig ON Artifacts(Platform, Configuration);
            ",
        },
    };
}
