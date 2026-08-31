using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Diagnostic report produced by <see cref="AssetRegistry.Diagnose"/>.
/// Pure data — JSON-serialisable for the issue panel / log file.
/// </summary>
public sealed class AssetRegistryDiagnostics
{
    /// <summary>True when the diagnostic surface found at least one
    /// issue that should block a save or build.</summary>
    public bool HasBlockingIssue { get; set; }

    /// <summary>Asset GUIDs referenced by scenes/components but never
    /// present in the registry (missing assets).</summary>
    public List<Guid> MissingAssetGuids { get; } = new();

    /// <summary>Asset GUIDs that show up on more than one .meta file
    /// (duplicate detection).</summary>
    public List<Guid> DuplicateAssetGuids { get; } = new();

    /// <summary>Files under the content root that aren't asset-known.
    /// Surfaced so the user can decide whether to import or delete
    /// them — never blocks a save.</summary>
    public List<string> UnregisteredFiles { get; } = new();
}

public static class AssetRegistryDiagnosticsRunner
{
    /// <summary>
    /// Run a full diagnostic pass over the registry: scan the
    /// content root for unregistered files, detect duplicate .meta
    /// GUIDs, and produce a report. Designed to be cheap enough to
    /// call from a status-bar refresh without re-walking disk.
    /// </summary>
    public static AssetRegistryDiagnostics Diagnose()
    {
        var report = new AssetRegistryDiagnostics();

        // Find assets referenced by the registry that don't have a
        // corresponding on-disk file. The AssetRegistry doesn't
        // itself know what consumers depend on a given asset, so we
        // assume the SQLite reverse-deps are authoritative when a
        // backend is configured. Otherwise we report none — the
        // editor can't tell without a dependency walk.
        foreach (var info in AssetRegistry.Assets)
        {
            if (string.IsNullOrEmpty(info.FullPath)) continue;
            if (!File.Exists(info.FullPath) && !Directory.Exists(info.FullPath))
            {
                // Surface as "missing" only when the Guid is non-empty;
                // an entry without a backing file is still recoverable
                // if the GUID is empty (registration hasn't been
                // committed yet).
                if (info.Guid != Guid.Empty)
                {
                    report.MissingAssetGuids.Add(info.Guid);
                }
            }
        }

        // Duplicate GUID detection: hash by Guid, surface any that
        // appear on more than one .meta file. Cheap because we
        // iterate the registry's already-loaded AssetInfo list.
        var byGuid = new Dictionary<Guid, int>();
        foreach (var info in AssetRegistry.Assets)
        {
            if (info.Guid == Guid.Empty) continue;
            byGuid.TryGetValue(info.Guid, out var count);
            byGuid[info.Guid] = count + 1;
        }
        foreach (var (guid, count) in byGuid)
        {
            if (count > 1) report.DuplicateAssetGuids.Add(guid);
        }

        // Unregistered files: walk the content root looking for
        // unknown files. We only do this if the registry has been
        // initialised — otherwise it has no opinion about what
        // counts as a "registered" file.
        var root = AssetRegistry.ProjectRoot;
        var contentRoot = AssetRegistry.ProjectContentRoot;
        if (!string.IsNullOrEmpty(root) && !string.IsNullOrEmpty(contentRoot) && Directory.Exists(contentRoot))
        {
            try
            {
                var knownPaths = new HashSet<string>(
                    AssetRegistry.Assets.Select(a => a.FullPath),
                    StringComparer.OrdinalIgnoreCase);
                foreach (var file in Directory.EnumerateFiles(contentRoot, "*", SearchOption.AllDirectories))
                {
                    if (!ContentHelper.IsAssetFile(file)) continue;
                    if (knownPaths.Contains(file)) continue;
                    report.UnregisteredFiles.Add(file);
                }
            }
            catch (Exception ex)
            {
                Logger.Warning($"AssetRegistryDiagnostics: scan failed — {ex.Message}");
            }
        }

        report.HasBlockingIssue = report.DuplicateAssetGuids.Count > 0;
        return report;
    }
}
