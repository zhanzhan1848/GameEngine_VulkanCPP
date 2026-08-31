using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// Extracts the fixed forward renderer's Albedo/Normal/ORM bindings from a
/// material graph. The material preview can execute arbitrary graph shader
/// code, while the World renderer consumes three texture slots; this
/// resolver walks the graph backwards from each output pin — through any
/// intermediate math nodes — to the first upstream ConstantTexture and
/// binds its asset path.
/// </summary>
public static class MaterialWorldTextureResolver
{
    public static string?[] Resolve(string materialPath)
    {
        var result = new string?[3];
        if (string.IsNullOrWhiteSpace(materialPath)) return result;
        var graphPath = Path.ChangeExtension(materialPath, ".material.graph.json");
        if (!File.Exists(graphPath)) return result;

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(graphPath));
            var root = document.RootElement;
            if (!root.TryGetProperty("nodes", out var nodes) || nodes.ValueKind != JsonValueKind.Array
                || !root.TryGetProperty("connections", out var connections)
                || connections.ValueKind != JsonValueKind.Array)
                return result;

            var nodeTypes = new List<string>();
            var texturePaths = new List<string?>();
            var outputNode = -1;
            var index = 0;
            foreach (var node in nodes.EnumerateArray())
            {
                var type = node.TryGetProperty("type", out var typeValue)
                    ? typeValue.GetString() ?? string.Empty
                    : string.Empty;
                nodeTypes.Add(type);
                string? texturePath = null;
                if (type == "ConstantTexture"
                    && node.TryGetProperty("params", out var parameters)
                    && parameters.TryGetProperty("asset_path", out var assetPath))
                    texturePath = assetPath.GetString();
                texturePaths.Add(texturePath);
                if (type == "MaterialOutput") outputNode = index;
                index++;
            }
            if (outputNode < 0) return result;

            var links = new List<Link>();
            foreach (var connection in connections.EnumerateArray())
            {
                links.Add(new Link(
                    connection.GetProperty("from_node").GetInt32(),
                    connection.GetProperty("from_pin").GetInt32(),
                    connection.GetProperty("to_node").GetInt32(),
                    connection.GetProperty("to_pin").GetInt32()));
            }

            // Prefer explicit texture pins. For the usual sampled BaseColor
            // graph, trace SampleTexture's texture input back to ConstantTexture.
            result[0] = ResolveOutputTexture(4) ?? ResolveOutputTexture(0);
            result[1] = ResolveOutputTexture(5);
            result[2] = ResolveOutputTexture(6);
            return result;

            string? ResolveOutputTexture(int outputPin)
            {
                var source = links.Find(link => link.ToNode == outputNode && link.ToPin == outputPin);
                if (source == default) return null;
                return ResolveNode(source.FromNode, new HashSet<int>());
            }

            // Generic backward data-flow walk: follow every input pin of any
            // node type until a ConstantTexture terminates the chain. Math
            // nodes between the sampler and MaterialOutput (Add/Lerp/Multiply,
            // …) must not break resolution — the fixed forward renderer only
            // needs one representative albedo/normal/ORM texture per slot.
            // Input pins are visited in ascending pin order so multi-input
            // nodes resolve deterministically (first input wins).
            string? ResolveNode(int nodeIndex, HashSet<int> visited)
            {
                if (nodeIndex < 0 || nodeIndex >= nodeTypes.Count || !visited.Add(nodeIndex)) return null;
                if (nodeTypes[nodeIndex] == "ConstantTexture")
                    return ResolveAssetPath(texturePaths[nodeIndex], materialPath);

                foreach (var link in links.Where(l => l.ToNode == nodeIndex).OrderBy(l => l.ToPin))
                {
                    var resolved = ResolveNode(link.FromNode, visited);
                    if (resolved != null) return resolved;
                }
                return null;
            }
        }
        catch
        {
            return result;
        }
    }

    private static string? ResolveAssetPath(string? assetPath, string materialPath)
    {
        if (string.IsNullOrWhiteSpace(assetPath)) return null;
        if (Path.IsPathRooted(assetPath)) return File.Exists(assetPath) ? assetPath : null;

        if (!string.IsNullOrWhiteSpace(AssetRegistry.ProjectRoot))
        {
            var projectPath = Path.GetFullPath(Path.Combine(
                AssetRegistry.ProjectRoot!,
                assetPath.Replace('/', Path.DirectorySeparatorChar)));
            if (File.Exists(projectPath)) return projectPath;
        }

        var localPath = Path.GetFullPath(Path.Combine(
            Path.GetDirectoryName(materialPath) ?? string.Empty,
            assetPath.Replace('/', Path.DirectorySeparatorChar)));
        return File.Exists(localPath) ? localPath : null;
    }

    private readonly record struct Link(int FromNode, int FromPin, int ToNode, int ToPin);
}
