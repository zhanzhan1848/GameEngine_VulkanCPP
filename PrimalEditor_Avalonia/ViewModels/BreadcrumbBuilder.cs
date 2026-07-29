using System;
using System.Collections.Generic;
using System.IO;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// One segment in a content browser breadcrumb — usually a folder
/// name. The full breadcrumb is built from a content root and a
/// selected subpath; the root segment is always emitted first.
/// </summary>
public sealed class BreadcrumbSegment
{
    public string Name { get; }
    public string FullPath { get; }
    public bool IsRoot { get; }
    public BreadcrumbSegment(string name, string fullPath, bool isRoot)
    {
        Name = name;
        FullPath = fullPath;
        IsRoot = isRoot;
    }
}

/// <summary>
/// Builds a breadcrumb list from a project content root and a
/// selected subpath. The root segment is always the first item; each
/// further segment corresponds to one directory step under the root.
/// An out-of-range subpath returns just the root segment so the UI
/// can still render a sensible state.
/// </summary>
public static class BreadcrumbBuilder
{
    public static IReadOnlyList<BreadcrumbSegment> Build(string contentRoot, string? selectedPath)
    {
        if (string.IsNullOrEmpty(contentRoot))
            return Array.Empty<BreadcrumbSegment>();

        var rootName = string.IsNullOrEmpty(contentRoot)
            ? "Content"
            : new DirectoryInfo(contentRoot).Name;
        if (string.IsNullOrEmpty(rootName)) rootName = "Content";
        var list = new List<BreadcrumbSegment> { new BreadcrumbSegment(rootName, contentRoot, true) };

        if (string.IsNullOrEmpty(selectedPath)) return list;
        if (string.Equals(Path.GetFullPath(selectedPath), Path.GetFullPath(contentRoot), StringComparison.OrdinalIgnoreCase))
            return list;

        var relative = selectedPath;
        if (relative.StartsWith(contentRoot, StringComparison.OrdinalIgnoreCase))
        {
            relative = relative.Substring(contentRoot.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
        else
        {
            return list;
        }
        if (string.IsNullOrEmpty(relative)) return list;

        var current = contentRoot;
        foreach (var part in relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
        {
            if (string.IsNullOrEmpty(part)) continue;
            current = Path.Combine(current, part);
            list.Add(new BreadcrumbSegment(part, current, false));
        }
        return list;
    }
}
