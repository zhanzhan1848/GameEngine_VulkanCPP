using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// End-to-end rebuild driver. Performs, in order:
///   1. Rescan the Content tree (and read .meta files) so the asset
///      registry reflects any on-disk changes.
///   2. Generate a <see cref="BuildManifest"/> from the default scene
///      dependency closure.
///   3. Persist the manifest to SQLite and to
///      .Primal/Build/{ConfigKey}.json.
/// Subsequent steps (cooking, packaging, platform upload) are stubs that
/// mark each manifest entry's artifact as <c>Pending</c> in SQLite; the
/// toolchain that actually cooks the assets will replace those rows
/// when it completes.
/// </summary>
public static class BuildPipeline
{
    public static string Platform { get; set; } = BuildManifestGenerator.DefaultPlatform;

    public static BuildManifest ExecuteRebuild(Project project)
    {
        if (project == null) throw new ArgumentNullException(nameof(project));

        Logger.Info($"[Build] Rescanning Content for project '{project.Name}'");
        AssetRegistry.Initialize(project.ContentPath, project.Path);

        var configuration = project.BuildConfigName;
        var manifest = BuildManifestGenerator.Refresh(project);

        PersistManifest(project, manifest);

        MarkArtifacts(project, manifest);
        Logger.Info($"[Build] Rebuild complete: {manifest.Entries.Count} asset(s) under {manifest.ConfigKey}");
        return manifest;
    }

    private static void PersistManifest(Project project, BuildManifest manifest)
    {
        if (project == null || string.IsNullOrEmpty(project.Path)) return;
        var json = JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true });

        // SQLite-backed mirror so other tools can read the manifest
        // without opening the editor's UI.
        var backend = AssetRegistrySnapshot.OpenSqliteBackendForProject(project);
        if (backend is SqliteAssetRegistryBackend sqlite)
        {
            try
            {
                sqlite.UpsertBuildManifest(manifest.ConfigKey, json, manifest.ProducedAtUtc);
            }
            catch (Exception ex)
            {
                Logger.Warning($"[Build] Failed to write manifest to SQLite: {ex.Message}");
            }
        }
    }

    private static void MarkArtifacts(Project project, BuildManifest manifest)
    {
        var backend = AssetRegistrySnapshot.OpenSqliteBackendForProject(project);
        if (backend is not SqliteAssetRegistryBackend sqlite) return;
        try
        {
            foreach (var entry in manifest.Entries)
            {
                sqlite.UpsertArtifact(
                    entry.AssetGuid.ToString(),
                    manifest.Platform,
                    manifest.Configuration,
                    cookedPath: string.Empty,
                    artifactHash: string.Empty,
                    producedAtUtc: manifest.ProducedAtUtc,
                    status: "PendingCook");
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"[Build] Failed to seed Artifact rows: {ex.Message}");
        }
    }
}

internal static class AssetRegistrySnapshot
{
    public static IAssetRegistryBackend? OpenSqliteBackendForProject(Project project)
    {
        if (project == null || string.IsNullOrEmpty(project.Path)) return null;
        var path = Path.Combine(project.Path, ".Primal", "AssetDatabase.sqlite");
        try
        {
            return new SqliteAssetRegistryBackend(path);
        }
        catch (Exception ex)
        {
            Logger.Warning($"AssetRegistrySnapshot: cannot open SQLite backend at {path}: {ex.Message}");
            return null;
        }
    }
}
