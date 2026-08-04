using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.GameProject.Migration;

public sealed class ProjectV1ToV2Migration : IProjectMigration
{
    private readonly IFileMover _files;

    public int SourceVersion => 1;
    public int TargetVersion => 2;

    public ProjectV1ToV2Migration() : this(new SystemFileMover()) { }
    public ProjectV1ToV2Migration(IFileMover files) { _files = files; }

    public ProjectMigrationResult Migrate(string projectFilePath)
    {
        var root = Path.GetDirectoryName(Path.GetFullPath(projectFilePath)) ?? throw new InvalidOperationException();
        var staging = Path.Combine(root, ".Primal", "Temp", "Migration", Guid.NewGuid().ToString("N"));
        var movedScenes = new List<(string from, string to)>();
        var backupPath = projectFilePath + ".v1." + DateTime.UtcNow.ToString("yyyyMMddHHmmssfff") + ".bak";

        try
        {
            var legacy = JsonSerializer.Deserialize<LegacyProject>(File.ReadAllText(projectFilePath), LegacyOptions)
                ?? throw new InvalidDataException("Legacy project document is empty.");
            if (legacy.Scenes.Count == 0) throw new InvalidDataException("Legacy project has no scenes.");

            // AssetRegistry must be initialised so v1 AssetPath strings
            // resolve to .meta GUIDs we can embed into MeshRenderer
            // components. The migration runs on the project root so we
            // treat that as both content root and project root.
            AssetRegistry.Initialize(Path.Combine(root, "Content"), root);

            var project = Project.CreateNew(Path.GetFileNameWithoutExtension(projectFilePath), root);
            project.BuildConfig = legacy.BuildConfig;
            Directory.CreateDirectory(Path.Combine(staging, "Scenes"));
            foreach (var oldScene in legacy.Scenes)
            {
                var scene = ConvertScene(oldScene, root);
                project.Scenes.Add(scene);
                if (project.ActiveScene is null || oldScene.IsActive) project.ActiveScene = scene;
                Serializer.SaveScene(scene, Path.Combine(staging, "Scenes", scene.Guid.Value.ToString("D") + Scene.Extension));
            }
            Serializer.SaveProject(project, Path.Combine(staging, Path.GetFileName(projectFilePath)));

            var stagedProject = Serializer.LoadProject(Path.Combine(staging, Path.GetFileName(projectFilePath)))
                ?? throw new InvalidDataException("Staged project failed validation.");
            foreach (var scene in stagedProject.Scenes)
            {
                var stagedScenePath = Path.Combine(staging, "Scenes", scene.Guid.Value.ToString("D") + Scene.Extension);
                if (Serializer.LoadScene(stagedScenePath) is null) throw new InvalidDataException("Staged scene failed validation.");
            }

            _files.Copy(projectFilePath, backupPath, overwrite: false);
            Directory.CreateDirectory(Path.Combine(root, "Scenes"));
            foreach (var sceneFile in Directory.GetFiles(Path.Combine(staging, "Scenes"), "*" + Scene.Extension))
            {
                var dest = Path.Combine(root, "Scenes", Path.GetFileName(sceneFile));
                _files.Move(sceneFile, dest, overwrite: true);
                movedScenes.Add((sceneFile, dest));
            }
            _files.Move(Path.Combine(staging, Path.GetFileName(projectFilePath)), projectFilePath, overwrite: true);
            return ProjectMigrationResult.Completed();
        }
        catch (Exception ex)
        {
            // Roll back partial state in reverse order. Already-moved
            // scenes return to staging; the .primal move either succeeded
            // (nothing to undo) or failed before the rename completed
            // (also nothing to undo). If the rename partially succeeded,
            // restore from backup.
            foreach (var (from, to) in movedScenes.AsEnumerable().Reverse())
            {
                try { _files.Move(to, from, overwrite: true); } catch { }
            }
            try { if (File.Exists(backupPath)) _files.Copy(backupPath, projectFilePath, overwrite: true); } catch { }
            return ProjectMigrationResult.Failed(ex.Message);
        }
        finally
        {
            try { if (Directory.Exists(staging)) Directory.Delete(staging, true); } catch { }
            try { if (File.Exists(backupPath) && !File.Exists(projectFilePath)) File.Delete(backupPath); } catch { }
        }
    }

    private static Scene ConvertScene(LegacyScene source, string projectRoot)
    {
        var scene = new Scene(source.Name);
        foreach (var oldEntity in source.Entities)
        {
            var entity = new GameEntity(oldEntity.Name)
            {
                IsEnabled = oldEntity.IsEnabled,
                ParentScene = scene,
            };

            // Preserve the original Transform component verbatim. v1
            // projects only stored a transform — runtime component
            // data lived entirely in the AssetPath reference.
            foreach (var oldComponent in oldEntity.Components)
            {
                if (oldComponent.TryGetProperty("$type", out var type) &&
                    string.Equals(type.GetString(), "transform", StringComparison.OrdinalIgnoreCase))
                {
                    entity.Components.Add(oldComponent.Deserialize<Transform>() ?? new Transform());
                }
            }
            // If the legacy entity had no explicit transform we still
            // need one — every GameEntity expects one for SceneRuntime
            // to read its position.
            if (entity.GetTransform() == null)
            {
                entity.Components.Add(new Transform());
            }

            // Translate v1 AssetPath into a MeshRendererComponent that
            // points at the asset's .meta GUID. The .meta is the
            // authority; if the asset file is missing we drop the
            // reference rather than silently lose it.
            var assetGuid = ResolveLegacyAssetGuid(oldEntity.AssetPath, projectRoot);
            if (assetGuid.HasValue)
            {
                var meshRenderer = new MeshRendererComponent { MeshGuid = assetGuid.Value };
                entity.Components.Add(meshRenderer);
            }
            scene.Entities.Add(entity);
        }
        return scene;
    }

    /// <summary>
    /// Map a v1 <c>Content/...</c> relative asset path to the asset's
    /// AssetGuid (read from its .meta sidecar). Returns null when the
    /// asset file is missing or unregistered so the migration can
    /// surface a warning instead of silently dropping references.
    /// </summary>
    private static AssetGuid? ResolveLegacyAssetGuid(string? legacyAssetPath, string projectRoot)
    {
        if (string.IsNullOrWhiteSpace(legacyAssetPath)) return null;
        var normalised = legacyAssetPath.Replace('\\', '/').TrimStart('/');
        var absolute = Path.Combine(projectRoot, normalised);
        // ResolveRelative: handle "../" inside legacyAssetPath.
        if (!Path.IsPathRooted(absolute))
        {
            absolute = Path.GetFullPath(Path.Combine(projectRoot, normalised));
        }
        if (!File.Exists(absolute))
        {
            Logger.Warning($"V1→V2 migration: asset path '{legacyAssetPath}' did not exist on disk; skipping reference");
            return null;
        }
        AssetRegistry.Register(absolute);
        var info = AssetRegistry.Get(absolute);
        return info == null ? null : new AssetGuid(info.Guid);
    }

    private static readonly JsonSerializerOptions LegacyOptions = new() { PropertyNameCaseInsensitive = true };

    private sealed class LegacyProject
    {
        public int BuildConfig { get; set; }
        public List<LegacyScene> Scenes { get; set; } = new();
    }

    private sealed class LegacyScene
    {
        public string Name { get; set; } = "Scene";
        public bool IsActive { get; set; }
        public List<LegacyEntity> Entities { get; set; } = new();
    }

    private sealed class LegacyEntity
    {
        public string Name { get; set; } = "Entity";
        public bool IsEnabled { get; set; } = true;
        public string? AssetPath { get; set; }
        public List<JsonElement> Components { get; set; } = new();
    }
}
