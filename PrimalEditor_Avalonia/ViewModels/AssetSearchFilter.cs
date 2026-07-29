using System;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Pure, allocation-free name filter for the content browser. The
/// browser holds the live asset list and a search text; callers call
/// <see cref="Match"/> per item to decide if it should appear in the
/// filtered view. Folder and file are matched the same way so the
/// caller can apply a single predicate to a mixed list.
/// </summary>
public static class AssetSearchFilter
{
    public static bool Match(string? displayName, string? query)
    {
        if (string.IsNullOrEmpty(query)) return true;
        if (string.IsNullOrEmpty(displayName)) return false;
        return displayName.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0;
    }
}
