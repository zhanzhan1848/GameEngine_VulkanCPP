using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// One-shot scan of recent projects for the newest eligible autosave
/// recovery candidate. The scan runs at most once per instance; the
/// result is cached so subsequent calls return the same candidate (or
/// null) without re-touching the filesystem. Scans each recent path
/// even when its live <c>.primal</c> file is missing — as long as the
/// project's autosave directory exists, recovery may still be possible.
/// </summary>
public sealed class AutoSaveRecoveryCoordinator
{
    private readonly IReadOnlyList<string> _projectPaths;
    private bool _scanned;
    private RecoveryCandidate? _cached;

    public AutoSaveRecoveryCoordinator(IReadOnlyList<string> projectPaths)
    {
        _projectPaths = projectPaths ?? Array.Empty<string>();
    }

    public RecoveryCandidate? ScanForRecoveryCandidates()
    {
        if (_scanned) return _cached;
        _scanned = true;

        RecoveryCandidate? best = null;
        foreach (var projectPath in _projectPaths)
        {
            if (string.IsNullOrEmpty(projectPath)) continue;
            var projectDir = Path.GetDirectoryName(projectPath);
            if (string.IsNullOrEmpty(projectDir)) continue;
            if (!Directory.Exists(projectDir)) continue;
            var autosaveDir = Path.Combine(projectDir, ".autosave");
            if (!Directory.Exists(autosaveDir)) continue;

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            if (bundles.Count == 0) continue;

            var eligible = AutoSaveRecoveryService.SelectNewestEligible(projectPath, bundles);
            if (eligible == null) continue;

            if (best == null || eligible.Timestamp > best.Bundle.Timestamp)
            {
                best = new RecoveryCandidate(eligible, projectPath);
            }
        }
        _cached = best;
        return best;
    }

    public sealed class RecoveryCandidate
    {
        public AutoSaveRecoveryService.AutoSaveBundle Bundle { get; }
        public string OriginalProjectFilePath { get; }

        public RecoveryCandidate(AutoSaveRecoveryService.AutoSaveBundle bundle, string originalProjectFilePath)
        {
            Bundle = bundle;
            OriginalProjectFilePath = originalProjectFilePath;
        }
    }
}
