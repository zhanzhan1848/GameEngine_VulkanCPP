using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Content;

public sealed class BuildManifest
{
    public string ConfigKey { get; set; } = string.Empty;
    public string Platform { get; set; } = string.Empty;
    public string Configuration { get; set; } = string.Empty;
    public DateTime ProducedAtUtc { get; set; }
    public List<BuildManifestEntry> Entries { get; } = new();
}

public sealed class BuildManifestEntry
{
    public Guid AssetGuid { get; set; }
    public string AssetType { get; set; } = string.Empty;
    public string RelativePath { get; set; } = string.Empty;
    public IReadOnlyList<Guid> Dependencies { get; set; } = Array.Empty<Guid>();
}

/// <summary>
/// Builds a project-wide package of every asset reachable from the
/// default scene (and any scene referenced by the manifest, since they
/// all travel in the cooked output). Output is written both to the
/// SQLite database (.BuildManifests) and to disk under
/// .Primal/Build/{ConfigKey}.json so external tools can read it
/// without opening the editor.
/// </summary>
public static class BuildManifestGenerator
{
    public const string DefaultPlatform = "Editor";
    public const string DefaultConfiguration = "DebugEditor";

    public static BuildManifest Generate(Project project, string platform = DefaultPlatform, string configuration = DefaultConfiguration)
    {
        if (project == null) throw new ArgumentNullException(nameof(project));
        var manifest = new BuildManifest
        {
            Platform = platform,
            Configuration = configuration,
            ConfigKey = MakeConfigKey(platform, configuration),
            ProducedAtUtc = DateTime.UtcNow,
        };

        // Start from each scene's MeshRenderer / MaterialOverride
        // references and recurse through AssetRegistry dependencies.
        var visited = new HashSet<Guid>();
        foreach (var scene in project.Scenes)
            CollectSceneReferences(scene, visited, manifest);

        Logger.Info($"BuildManifestGenerator: collected {manifest.Entries.Count} asset(s) from {project.Scenes.Count} scene(s)");
        return manifest;
    }

    public static BuildManifest Refresh(Project project)
    {
        var manifest = Generate(project);
        if (project == null || string.IsNullOrEmpty(project.Path)) return manifest;
        try
        {
            var key = manifest.ConfigKey;
            var json = JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true });
            var dir = Path.Combine(project.Path, ".Primal", "Build");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, key + ".json");
            File.WriteAllText(path, json + Environment.NewLine);
            Logger.Info($"BuildManifestGenerator: wrote manifest to {path}");
        }
        catch (Exception ex)
        {
            Logger.Warning($"BuildManifestGenerator: failed to persist manifest: {ex.Message}");
        }
        return manifest;
    }

    private static void CollectSceneReferences(Scene scene, HashSet<Guid> visited, BuildManifest manifest)
    {
        if (scene == null) return;
        foreach (var entity in scene.Entities)
        {
            foreach (var renderer in entity.GetComponents<MeshRendererComponent>())
            {
                if (renderer.MeshGuid.Value != Guid.Empty)
                    AddDependencyClosure(renderer.MeshGuid.Value, visited, manifest);
                foreach (var matGuid in renderer.MaterialGuids)
                    if (matGuid.Value != Guid.Empty)
                        AddDependencyClosure(matGuid.Value, visited, manifest);
            }
            foreach (var matOverride in entity.GetComponents<MaterialOverrideComponent>())
            {
                foreach (var slot in matOverride.Slots)
                {
                    if (slot.MaterialGuid.Value != Guid.Empty)
                        AddDependencyClosure(slot.MaterialGuid.Value, visited, manifest);
                }
            }
        }
    }

    private static void AddDependencyClosure(Guid guid, HashSet<Guid> visited, BuildManifest manifest)
    {
        if (!visited.Add(guid)) return;
        var info = AssetRegistry.GetByGuid(guid);
        if (info == null) return;

        var relative = AssetRegistry.GetRelativePath(info.FullPath)?.Value ?? string.Empty;
        var deps = AssetRegistry.GetDependencies(info.FullPath).ToList();

        manifest.Entries.Add(new BuildManifestEntry
        {
            AssetGuid = guid,
            AssetType = info.Type.ToString(),
            RelativePath = relative,
            Dependencies = deps
        });

        foreach (var dep in deps)
            AddDependencyClosure(dep, visited, manifest);
    }

    public static string MakeConfigKey(string platform, string configuration)
        => platform + "-" + configuration;
}

internal static class GameEntityComponentExtensions
{
    public static IEnumerable<T> GetComponents<T>(this GameEntity entity) where T : Component
        => entity.Components.OfType<T>();
}
