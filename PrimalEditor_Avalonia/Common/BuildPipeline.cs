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
            var cookedCount = 0;
            foreach (var entry in manifest.Entries)
            {
                // 实际 cooked 产物状态（asset-pipeline S1）：能解析出可用的产物
                // 文件（cooked 目录 / sidecar 旁 .mesh / 源文件回退——.lua 等
                // 免 cook 类型）记 Cooked+SHA1，否则 PendingCook。清单不再把
                // "待 cook 占位"当成产物状态——离线重建清单 ≠ 完整烘焙包。
                var info = AssetRegistry.GetByGuid(entry.AssetGuid);
                string cookedPath = string.Empty, hash = string.Empty;
                string status = "PendingCook";
                if (info != null)
                {
                    var resolved = Native.CookedArtifactResolver.ResolveCookedPath(
                        info, manifest.Platform, manifest.Configuration, useSourceFallback: true);
                    if (!string.IsNullOrEmpty(resolved) && File.Exists(resolved))
                    {
                        cookedPath = resolved;
                        hash = Convert.ToHexString(
                            System.Security.Cryptography.SHA1.HashData(File.ReadAllBytes(resolved)));
                        status = "Cooked";
                        ++cookedCount;
                    }
                }
                sqlite.UpsertArtifact(
                    entry.AssetGuid.ToString(),
                    manifest.Platform,
                    manifest.Configuration,
                    cookedPath: cookedPath,
                    artifactHash: hash,
                    producedAtUtc: manifest.ProducedAtUtc,
                    status: status);
            }
            Logger.Info($"[Build] Artifact rows: {cookedCount} cooked, " +
                        $"{manifest.Entries.Count - cookedCount} pending cook");
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
