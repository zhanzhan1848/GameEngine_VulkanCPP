using System;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Geometry;

/// <summary>
/// Reimports a cooked <c>.mesh</c> from a tracked source file.
///
/// Workflow:
///   1. Validate the source file exists and is newer than the current
///      cooked mesh (or the sidecar says we have never cooked it).
///   2. Cook the source into a sibling <c>.partial.mesh</c> next to
///      the destination so a crash mid-cook never leaves a half-written
///      <c>.mesh</c>.
///   3. Load the partial mesh with <see cref="Geometry"/> to confirm
///      it is well-formed; reject the reimport if it is empty or
///      unparseable.
///   4. Atomically replace the destination by deleting the old file
///      and renaming the partial into place.
///
/// On any failure, the original <c>.mesh</c> is left untouched.
/// </summary>
public sealed class GeometryReimportService
{
    public ReimportResult Reimport(string sourcePath, string destinationMeshPath)
    {
        if (string.IsNullOrEmpty(sourcePath))
            return new ReimportResult(ReimportStatus.MissingSource, "Source path was empty");
        if (!File.Exists(sourcePath))
            return new ReimportResult(ReimportStatus.MissingSource, $"Source not found: {sourcePath}");
        if (string.IsNullOrEmpty(destinationMeshPath))
            return new ReimportResult(ReimportStatus.CookFailed, "Destination mesh path was empty");

        var dir = Path.GetDirectoryName(destinationMeshPath);
        if (string.IsNullOrEmpty(dir))
            return new ReimportResult(ReimportStatus.CookFailed, $"Bad destination directory: {destinationMeshPath}");
        Directory.CreateDirectory(dir);

        var partial = destinationMeshPath + ".partial";

        try
        {
            if (File.Exists(partial)) File.Delete(partial);
        }
        catch (Exception ex)
        {
            return new ReimportResult(ReimportStatus.CookFailed, $"Failed to clear stale partial: {ex.Message}");
        }

        if (!NativeEngineBridge.CookMesh(sourcePath, partial))
            return new ReimportResult(ReimportStatus.CookFailed, $"CookMesh returned false for {sourcePath}");

        if (!File.Exists(partial))
            return new ReimportResult(ReimportStatus.CookFailed, "Cook did not produce an output file");

        var probe = new Geometry();
        if (!probe.Load(partial) || probe.LODs.Count == 0)
        {
            TryDelete(partial);
            return new ReimportResult(ReimportStatus.InvalidOutput, "Cooked mesh failed to load or contains no LODs");
        }

        if (File.Exists(destinationMeshPath) && FileEquals(destinationMeshPath, partial))
        {
            TryDelete(partial);
            return new ReimportResult(ReimportStatus.NoChange, "Cooked output identical to current mesh");
        }

        try
        {
            if (File.Exists(destinationMeshPath)) File.Delete(destinationMeshPath);
            File.Move(partial, destinationMeshPath);
        }
        catch (Exception ex)
        {
            TryDelete(partial);
            return new ReimportResult(ReimportStatus.CookFailed, $"Failed to swap cooked mesh into place: {ex.Message}");
        }

        var info = new FileInfo(destinationMeshPath);
        return new ReimportResult(ReimportStatus.Success, "Reimported", destinationMeshPath, info.Length);
    }

    private static bool FileEquals(string a, string b)
    {
        var ai = new FileInfo(a);
        var bi = new FileInfo(b);
        if (ai.Length != bi.Length) return false;
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        var bufA = new byte[8192];
        var bufB = new byte[8192];
        int n;
        while ((n = fa.Read(bufA, 0, bufA.Length)) > 0)
        {
            fb.Read(bufB, 0, n);
            for (int i = 0; i < n; ++i)
            {
                if (bufA[i] != bufB[i]) return false;
            }
        }
        return true;
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (Exception ex) { Logger.Warning($"GeometryReimportService: failed to delete {path} — {ex.Message}"); }
    }
}
