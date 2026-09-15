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
/// .texture, .material, .material.json, .shader) are first-class
/// citizens and may be inspected directly.
/// </summary>
public static class ContentBrowserFilter
{
    private const string ModelSidecarSuffix = ".model.json";
    private const string MaterialSidecarSuffix = ".material.json";
    private const string ParticlePresetSidecarSuffix = ".particle.json";

    private static readonly HashSet<string> EngineFormats = new(StringComparer.OrdinalIgnoreCase)
    {
        ".model",
        ".mesh",
        ".texture",
        ".material",
        ".shader",
        ".pfx_emitter",
        // .material.json / .model.json / .particle.json handled via suffix checks
        // (Path.GetExtension only returns the last suffix, so multi-
        // extension sidecars need the EndsWith fast-path below).
    };

    public static bool IsEngineFormat(string fullPath)
    {
        if (string.IsNullOrEmpty(fullPath)) return false;
        // Multi-extension sidecars first — Path.GetExtension returns
        // ".json" for ".material.json", which would otherwise miss
        // the .material extension in EngineFormats above.
        if (fullPath.EndsWith(ModelSidecarSuffix, StringComparison.OrdinalIgnoreCase))
            return true;
        if (fullPath.EndsWith(MaterialSidecarSuffix, StringComparison.OrdinalIgnoreCase))
            return true;
        if (fullPath.EndsWith(ParticlePresetSidecarSuffix, StringComparison.OrdinalIgnoreCase))
            return true;
        return EngineFormats.Contains(Path.GetExtension(fullPath));
    }

    public static IEnumerable<AssetInfo> Apply(IEnumerable<AssetInfo> assets)
        => assets.Where(a => a.IsFolder || IsEngineFormat(a.FullPath));
}