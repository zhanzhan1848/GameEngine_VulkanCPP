using System;
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;

namespace PrimalEditor_Avalonia.Content;

public static class AssetDependencyExtractor
{
    public static IEnumerable<Guid> ExtractMaterialDependencies(string materialPath)
    {
        var document = MaterialSerializer.Read(materialPath);
        if (document is null) yield break;
        foreach (var dep in document.EnumerateDependencies())
            yield return dep.Value;
    }

    public static IEnumerable<string> ExtractShaderDependencies(string shaderPath)
    {
        var document = ShaderSerializer.Read(shaderPath);
        if (document is null) yield break;
        foreach (var dep in document.EnumerateDependencies())
            yield return dep;
    }

    public static IEnumerable<Guid> ExtractByExtension(string assetPath)
    {
        if (!File.Exists(assetPath)) yield break;
        var ext = Path.GetExtension(assetPath).ToLowerInvariant();
        if (ext == ".material")
        {
            foreach (var guid in ExtractMaterialDependencies(assetPath)) yield return guid;
        }
        else if (ext == ".shader")
        {
            foreach (var guid in ExtractShaderDependencies(assetPath))
                if (Guid.TryParse(guid, out var parsed)) yield return parsed;
        }
    }
}
