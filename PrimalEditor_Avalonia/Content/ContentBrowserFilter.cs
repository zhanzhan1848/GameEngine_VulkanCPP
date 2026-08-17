using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Restricts the Content Browser to engine-format assets only. Source
/// formats (.fbx, .obj, .gltf, .glb, .dae, .blend) are intentionally
/// excluded — they must flow through the Import pipeline before they
/// can be edited. Engine-format assets (.model.json, .model, .mesh,
/// .texture) are first-class citizens and may be inspected directly.
/// </summary>
public static class ContentBrowserFilter
{
    private const string ModelSidecarSuffix = ".model.json";

    private static readonly HashSet<string> EngineFormats = new(StringComparer.OrdinalIgnoreCase)
    {
        ".model",
        ".mesh",
        ".texture",
        // Future: ".shader", ".material.json"
    };

    public static bool IsEngineFormat(string fullPath)
    {
        if (string.IsNullOrEmpty(fullPath)) return false;
        if (fullPath.EndsWith(ModelSidecarSuffix, StringComparison.OrdinalIgnoreCase))
            return true;
        return EngineFormats.Contains(Path.GetExtension(fullPath));
    }

    public static IEnumerable<AssetInfo> Apply(IEnumerable<AssetInfo> assets)
        => assets.Where(a => a.IsFolder || IsEngineFormat(a.FullPath));
}