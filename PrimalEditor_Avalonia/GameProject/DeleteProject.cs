using System;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

/// <summary>
/// Permanent removal of a project from disk. This is the one place in the
/// editor that recursively deletes a user-authored directory, so every
/// entry point funnels through <see cref="ResolveProjectRoot"/> first:
/// we only ever delete a folder we can positively identify as a project
/// root by finding the .primal file sitting directly inside it.
/// </summary>
public static class DeleteProject
{
    /// <summary>
    /// Map a .primal file path to the project folder that may be deleted,
    /// or null when the path fails any safety check. Callers must treat a
    /// null result as "delete nothing".
    /// </summary>
    public static string? ResolveProjectRoot(string projectFilePath)
    {
        if (string.IsNullOrWhiteSpace(projectFilePath))
        {
            return null;
        }

        if (!string.Equals(Path.GetExtension(projectFilePath), Project.Extension,
                StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        string fullFilePath;
        try
        {
            fullFilePath = Path.GetFullPath(projectFilePath);
        }
        catch (Exception ex)
        {
            Logger.Warning($"Cannot resolve project path '{projectFilePath}': {ex.Message}");
            return null;
        }

        if (!File.Exists(fullFilePath))
        {
            return null;
        }

        var root = Path.GetDirectoryName(fullFilePath);
        if (string.IsNullOrEmpty(root) || !Directory.Exists(root))
        {
            return null;
        }

        // No parent => a volume root such as "/" or "C:\".
        if (Directory.GetParent(root) == null)
        {
            Logger.Warning($"Refusing to delete volume root '{root}'.");
            return null;
        }

        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (!string.IsNullOrEmpty(home) &&
            string.Equals(
                Path.TrimEndingDirectorySeparator(Path.GetFullPath(home)),
                Path.TrimEndingDirectorySeparator(root),
                StringComparison.OrdinalIgnoreCase))
        {
            Logger.Warning($"Refusing to delete home directory '{root}'.");
            return null;
        }

        return root;
    }

    /// <summary>
    /// Delete the entire project folder for the given .primal file.
    /// Returns true when the folder was removed. Returns false — without
    /// throwing — when the path fails a safety check, so callers that are
    /// cleaning up a recent-projects list can still drop the stale entry.
    /// </summary>
    public static bool DeleteProjectFolder(string projectFilePath)
    {
        var root = ResolveProjectRoot(projectFilePath);
        if (root == null)
        {
            Logger.Warning(
                $"Not deleting files for '{projectFilePath}': it does not resolve to a project folder.");
            return false;
        }

        try
        {
            Directory.Delete(root, recursive: true);
            Logger.Info($"Deleted project folder: {root}");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to delete project folder '{root}'", ex);
            throw;
        }
    }
}
