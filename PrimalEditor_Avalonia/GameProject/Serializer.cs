using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.GameProject;

public static class Serializer
{
    public static void SaveProject(Project project, string? path = null)
    {
        ArgumentNullException.ThrowIfNull(project);
        var filePath = path ?? project.FullPath;
        var document = ToDocument(project);
        SaveProjectDocument(document, filePath);
    }

    internal static void SaveProjectDocument(ProjectDocument document, string filePath)
    {
        ArgumentNullException.ThrowIfNull(document);
        var partialPath = filePath + ".partial";
        if (!document.IsValid) throw new InvalidDataException("Project manifest is invalid.");

        try
        {
            var directory = Path.GetDirectoryName(filePath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var json = JsonSerializer.Serialize(document, ProjectDocument.JsonOptions) + Environment.NewLine;
            using (var stream = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }

            var validation = JsonSerializer.Deserialize<ProjectDocument>(File.ReadAllText(partialPath), ProjectDocument.JsonOptions);
            if (validation is not { IsValid: true }) throw new InvalidDataException("Project manifest validation failed.");
            File.Move(partialPath, filePath, overwrite: true);
            Logger.Info($"Project saved: {filePath}");
        }
        catch
        {
            try { if (File.Exists(partialPath)) File.Delete(partialPath); } catch { }
            throw;
        }
    }

    public static Project? LoadProject(string filePath)
    {
        if (!File.Exists(filePath))
        {
            Logger.Error($"Project file not found: {filePath}");
            return null;
        }

        try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(filePath));
            if (!json.RootElement.TryGetProperty("SchemaVersion", out var version) ||
                version.GetInt32() != ProjectDocument.CurrentSchemaVersion)
            {
                Logger.Warning($"Project requires migration before loading: {filePath}");
                return null;
            }

            var document = json.RootElement.Deserialize<ProjectDocument>(ProjectDocument.JsonOptions);
            if (document is not { IsValid: true }) return null;
            return FromDocument(document, filePath);
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to load project: {filePath}", ex);
            return null;
        }
    }

    public static ProjectDocument ToDocument(Project project)
    {
        var sceneGuids = project.Scenes.Select(scene => scene.Guid).ToList();
        return new ProjectDocument
        {
            ProjectGuid = project.Guid,
            Name = project.Name,
            BuildConfig = project.BuildConfigName,
            DefaultScene = project.ActiveScene?.Guid,
            Scenes = sceneGuids,
            RenderSettings = new RenderSettingsDocument()
        };
    }

    /// <summary>
    /// Build a <see cref="Project"/> skeleton from a manifest without
    /// touching the disk for the per-scene payloads. Each entry in
    /// <see cref="ProjectDocument.Scenes"/> becomes a placeholder
    /// <see cref="Scene"/> with an empty entity list; the caller is
    /// responsible for filling them in via
    /// <see cref="LoadProjectScenesAsync"/> (or the synchronous
    /// <see cref="LoadProjectScenes"/>) right after this returns.
    /// </summary>
    public static Project FromDocument(ProjectDocument document, string filePath)
    {
        var fullPath = Path.GetFullPath(filePath);
        var project = new Project
        {
            Guid = document.ProjectGuid,
            CurrentSchemaVersion = document.SchemaVersion,
            Name = Path.GetFileNameWithoutExtension(fullPath),
            Path = Path.GetDirectoryName(fullPath) ?? string.Empty,
            BuildConfigName = document.BuildConfig
        };

        foreach (var sceneGuid in document.Scenes)
            project.Scenes.Add(new Scene { Guid = sceneGuid, Name = sceneGuid.Value.ToString("D") });

        if (document.DefaultScene is { } defaultGuid)
            project.ActiveScene = project.Scenes.FirstOrDefault(scene => scene.Guid == defaultGuid);

        return project;
    }

    /// <summary>
    /// Convert a v2 scene guid to its on-disk path inside the project's
    /// Scenes directory. The file name is the guid + ".scene"; the
    /// directory is supplied by the caller so this stays a pure
    /// projection.
    /// </summary>
    public static string ScenePath(Project project, SceneGuid guid)
        => Path.Combine(project.ScenesPath, guid.Value.ToString("D") + Scene.Extension);

    /// <summary>
    /// Populate every placeholder <see cref="Scene"/> in <paramref name="project"/>
    /// by deserializing the matching <c>{guid}.scene</c> file under
    /// <see cref="Project.ScenesPath"/>. Scenes whose file is missing
    /// are logged and left as empty placeholders so the project still
    /// opens. Failures are logged and skipped — opening a half-broken
    /// project is preferable to refusing to open entirely.
    /// </summary>
    public static void LoadProjectScenes(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        if (string.IsNullOrEmpty(project.Path)) return;

        for (var i = 0; i < project.Scenes.Count; i++)
        {
            var scene = project.Scenes[i];
            var path = ScenePath(project, scene.Guid);
            if (!File.Exists(path))
            {
                Logger.Warning($"Scene payload missing, keeping empty placeholder: {path}");
                continue;
            }

            try
            {
                var loaded = LoadScene(path);
                if (loaded == null)
                {
                    Logger.Warning($"Scene payload unreadable, keeping empty placeholder: {path}");
                    continue;
                }
                // Preserve the placeholder's position so that
                // WorldEditorViewModel.ActiveScene setter (which
                // compares by reference) keeps working. We swap the
                // placeholder's content in-place.
                scene.Name = loaded.Name;
                scene.Entities.Clear();
                foreach (var entity in loaded.Entities)
                    scene.Entities.Add(entity);
                CopyEnvironment(loaded.Environment, scene.Environment);
            scene.LightmapPage = loaded.LightmapPage;
                scene.EditorCamera.CopyFrom(loaded.EditorCamera);
            }
            catch (Exception ex)
            {
                Logger.Warning($"Scene payload failed to load ({path}): {ex.Message}");
            }
        }
    }

    public static SceneDocument ToSceneDocument(Scene scene)
    {
        var entities = new List<EntityDocument>();
        var seen = new HashSet<EntityGuid>();
        FlattenEntities(scene.Entities, null, seen, entities);

        return new SceneDocument
        {
            SceneGuid = scene.Guid,
            Name = scene.Name,
            Entities = entities,
            Environment = ToEnvironmentDocument(scene.Environment),
            LightmapPage = scene.LightmapPage,
            EditorCamera = ToEditorCameraDocument(scene.EditorCamera),
        };
    }

    /// <summary>
    /// Serialize entity subtrees to <see cref="EntityDocument"/>s without a
    /// wrapping Scene — the prefab creation path (plan §6.2 A4). Children
    /// are flattened depth-first exactly like <see cref="ToSceneDocument"/>.
    /// </summary>
    public static List<EntityDocument> ToEntityDocuments(IEnumerable<GameEntity> roots)
    {
        var entities = new List<EntityDocument>();
        FlattenEntities(roots, null, new HashSet<EntityGuid>(), entities);
        return entities;
    }

    /// <summary>
    /// Create live entities from <see cref="EntityDocument"/>s (the prefab
    /// instantiation path). With <paramref name="reassignGuids"/> every
    /// entity gets a fresh EntityGuid — two instances of the same prefab
    /// must never share identity in one scene. Returns the root entities
    /// (ParentEntityGuid == null) in document order; children are attached
    /// via Parent/Children but the caller must add roots to the scene.
    /// </summary>
    public static List<GameEntity> InstantiateEntityDocuments(
        IReadOnlyList<EntityDocument> documents, bool reassignGuids = false)
    {
        var map = new Dictionary<EntityGuid, GameEntity>();
        var roots = new List<GameEntity>();

        foreach (var doc in documents)
        {
            var entity = new GameEntity(doc.Name)
            {
                // Fresh guid per instance (property initializer generates one);
                // only scene loads keep the original identity.
                EntityGuid = reassignGuids ? new EntityGuid(Guid.NewGuid()) : doc.EntityGuid,
                IsEnabled = doc.IsEnabled,
            };
            map[doc.EntityGuid] = entity;
        }

        foreach (var doc in documents)
        {
            var entity = map[doc.EntityGuid];
            foreach (var componentDoc in doc.Components)
            {
                var component = DeserializeComponent(componentDoc);
                if (component != null) entity.Components.Add(component);
            }
            if (doc.ParentEntityGuid is { } parentGuid && map.TryGetValue(parentGuid, out var parent))
            {
                entity.Parent = parent;
                parent.Children.Add(entity);
            }
            else
            {
                roots.Add(entity);
            }
        }
        return roots;
    }


    private static EditorCameraDocument ToEditorCameraDocument(EditorCameraState camera) => new()
    {
        TargetX = camera.TargetX,
        TargetY = camera.TargetY,
        TargetZ = camera.TargetZ,
        Distance = camera.Distance,
        Yaw = camera.Yaw,
        Pitch = camera.Pitch,
    };

    private static SceneEnvironmentDocument ToEnvironmentDocument(SceneEnvironment env)
    {
        if (env == null) return new SceneEnvironmentDocument();
        return new SceneEnvironmentDocument
        {
            AmbientR = env.AmbientR,
            AmbientG = env.AmbientG,
            AmbientB = env.AmbientB,
            AmbientIntensity = env.AmbientIntensity,
            FogEnabled = env.FogEnabled,
            FogR = env.FogR,
            FogG = env.FogG,
            FogB = env.FogB,
            FogNear = env.FogNear,
            FogFar = env.FogFar,
            FogDensity = env.FogDensity,
            SkyboxEnabled = env.SkyboxEnabled,
            SkyR = env.SkyR,
            SkyG = env.SkyG,
            SkyB = env.SkyB,
            ToneMapping = env.ToneMapping.ToString(),
            Exposure = env.Exposure,
            MainCameraEntityGuid = env.MainCameraEntityGuid?.ToString("D"),
            EnvMapPath = env.EnvMapPath,
            StaticGIFrozen = env.StaticGIFrozen,
        };
    }

    private static void FlattenEntities(IEnumerable<GameEntity> entities, EntityGuid? parentGuid, HashSet<EntityGuid> seen, List<EntityDocument> output)
    {
        foreach (var entity in entities)
        {
            // Editor mutations (OnAddEntityClick) can leave a child on both
            // scene.Entities and parent.Children simultaneously. Dedupe by
            // EntityGuid so the file is written exactly once.
            if (!seen.Add(entity.EntityGuid)) continue;

            var entityDoc = new EntityDocument
            {
                EntityGuid = entity.EntityGuid,
                Name = entity.Name,
                IsEnabled = entity.IsEnabled,
                ParentEntityGuid = parentGuid,
                Components = new List<ComponentDocument>()
            };

            foreach (var component in entity.Components)
            {
                var componentJson = component switch
                {
                    Transform transform => JsonSerializer.SerializeToElement(transform, SceneDocument.JsonOptions),
                    MeshRendererComponent renderer => JsonSerializer.SerializeToElement(renderer, SceneDocument.JsonOptions),
                    MaterialOverrideComponent overrideComp => JsonSerializer.SerializeToElement(overrideComp, SceneDocument.JsonOptions),
                    LightComponent light => JsonSerializer.SerializeToElement(light, SceneDocument.JsonOptions),
                    EnvironmentComponent environment => JsonSerializer.SerializeToElement(environment, SceneDocument.JsonOptions),
                    CameraComponent camera => JsonSerializer.SerializeToElement(camera, SceneDocument.JsonOptions),
                    EntityMetadata metadata => JsonSerializer.SerializeToElement(metadata, SceneDocument.JsonOptions),
                    RotateScriptComponent rotate => JsonSerializer.SerializeToElement(rotate, SceneDocument.JsonOptions),
                    PatrolScriptComponent patrol => JsonSerializer.SerializeToElement(patrol, SceneDocument.JsonOptions),
                    LuaScriptComponent luaScript => JsonSerializer.SerializeToElement(luaScript, SceneDocument.JsonOptions),
                    AudioSourceComponent audioSource => JsonSerializer.SerializeToElement(audioSource, SceneDocument.JsonOptions),
                    NavAgentComponent navAgent => JsonSerializer.SerializeToElement(navAgent, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.PrefabInstanceComponent prefabInstance => JsonSerializer.SerializeToElement(prefabInstance, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.ParticleEmitterComponent particleEmitter => JsonSerializer.SerializeToElement(particleEmitter, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.CrossSceneReferenceComponent crossScene => JsonSerializer.SerializeToElement(crossScene, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.ColliderComponent collider => JsonSerializer.SerializeToElement(collider, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.RigidBodyComponent rigidBody => JsonSerializer.SerializeToElement(rigidBody, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.ParticleFXComponent particleFx => JsonSerializer.SerializeToElement(particleFx, SceneDocument.JsonOptions),
                    PrimalEditor_Avalonia.Components.GameplayRulesComponent gameplayRules => JsonSerializer.SerializeToElement(gameplayRules, SceneDocument.JsonOptions),
                    _ => throw new NotSupportedException($"Unknown component type: {component.GetType().Name}")
                };
                var componentDoc = new ComponentDocument
                {
                    Type = GetComponentTypeDiscriminator(component),
                    Data = componentJson
                };
                entityDoc.Components.Add(componentDoc);
            }

            output.Add(entityDoc);

            if (entity.Children.Count > 0)
            {
                FlattenEntities(entity.Children, entity.EntityGuid, seen, output);
            }
        }
    }

    private static string GetComponentTypeDiscriminator(Component component)
    {
        return component switch
        {
            Transform => "transform",
            MeshRendererComponent => "meshRenderer",
            MaterialOverrideComponent => "materialOverride",
            LightComponent => "light",
            EnvironmentComponent => "environment",
            CameraComponent => "camera",
            EntityMetadata => "metadata",
            RotateScriptComponent => "scriptRotate",
            PatrolScriptComponent => "scriptPatrol",
            LuaScriptComponent => "scriptLua",
            AudioSourceComponent => "audioSource",
            NavAgentComponent => "navAgent",
            PrimalEditor_Avalonia.Components.PrefabInstanceComponent => "prefabInstance",
            PrimalEditor_Avalonia.Components.ParticleEmitterComponent => "particleEmitter",
            PrimalEditor_Avalonia.Components.CrossSceneReferenceComponent => "crossSceneReference",
            PrimalEditor_Avalonia.Components.ColliderComponent => "collider",
            PrimalEditor_Avalonia.Components.RigidBodyComponent => "rigidbody",
            PrimalEditor_Avalonia.Components.ParticleFXComponent => "particleFX",
            PrimalEditor_Avalonia.Components.GameplayRulesComponent => "gameplayRules",
            _ => throw new NotSupportedException($"Unknown component type: {component.GetType().Name}")
        };
    }

    public static Scene FromSceneDocument(SceneDocument document)
    {
        var scene = new Scene(document.Name)
        {
            Guid = document.SceneGuid
        };

        ApplyEnvironment(scene.Environment, document.Environment);
        ApplyEditorCamera(scene.EditorCamera, document.EditorCamera);

        var entityMap = new Dictionary<EntityGuid, GameEntity>();

        foreach (var entityDoc in document.Entities)
        {
            if (entityMap.ContainsKey(entityDoc.EntityGuid))
            {
                throw new InvalidOperationException($"Duplicate EntityGuid detected: {entityDoc.EntityGuid}");
            }

            var entity = new GameEntity(entityDoc.Name)
            {
                EntityGuid = entityDoc.EntityGuid,
                IsEnabled = entityDoc.IsEnabled,
                ParentScene = scene
            };

            foreach (var componentDoc in entityDoc.Components)
            {
                var component = DeserializeComponent(componentDoc);
                if (component != null)
                {
                    entity.Components.Add(component);
                }
            }

            entityMap[entityDoc.EntityGuid] = entity;
            if (entityDoc.ParentEntityGuid is null) scene.Entities.Add(entity);
        }

        ValidateAndBuildHierarchy(document, entityMap);

        return scene;
    }

    private static void ApplyEnvironment(SceneEnvironment target, SceneEnvironmentDocument? source)
    {
        if (source == null) return;
        target.AmbientR = source.AmbientR;
        target.AmbientG = source.AmbientG;
        target.AmbientB = source.AmbientB;
        target.AmbientIntensity = source.AmbientIntensity;
        target.FogEnabled = source.FogEnabled;
        target.FogR = source.FogR;
        target.FogG = source.FogG;
        target.FogB = source.FogB;
        target.FogNear = source.FogNear;
        target.FogFar = source.FogFar;
        target.FogDensity = source.FogDensity;
        target.SkyboxEnabled = source.SkyboxEnabled;
        target.SkyR = source.SkyR;
        target.SkyG = source.SkyG;
        target.SkyB = source.SkyB;
        if (Enum.TryParse<ToneMappingMode>(source.ToneMapping, out var tm)) target.ToneMapping = tm;
        target.Exposure = source.Exposure;
        target.MainCameraEntityGuid = Guid.TryParse(source.MainCameraEntityGuid, out var mainCameraGuid)
            ? mainCameraGuid
            : null;
        target.EnvMapPath = source.EnvMapPath ?? string.Empty;
        target.StaticGIFrozen = source.StaticGIFrozen;
    }

    private static void CopyEnvironment(SceneEnvironment source, SceneEnvironment target)
        => ApplyEnvironment(target, ToEnvironmentDocument(source));

    private static void ApplyEditorCamera(EditorCameraState target, EditorCameraDocument? source)
    {
        if (source == null) return;
        target.TargetX = source.TargetX;
        target.TargetY = source.TargetY;
        target.TargetZ = source.TargetZ;
        target.Distance = source.Distance > 0f ? source.Distance : 12f;
        target.Yaw = source.Yaw;
        target.Pitch = source.Pitch;
    }

    private static Component? DeserializeComponent(ComponentDocument componentDoc)
    {
        return componentDoc.Type.ToLowerInvariant() switch
        {
            "transform" => JsonSerializer.Deserialize<Transform>(componentDoc.Data.GetRawText()),
            "meshrenderer" => JsonSerializer.Deserialize<MeshRendererComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "materialoverride" => JsonSerializer.Deserialize<MaterialOverrideComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "light" => JsonSerializer.Deserialize<LightComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "environment" => JsonSerializer.Deserialize<EnvironmentComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "camera" => JsonSerializer.Deserialize<CameraComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "metadata" => JsonSerializer.Deserialize<EntityMetadata>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "scriptrotate" => JsonSerializer.Deserialize<RotateScriptComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "scriptpatrol" => JsonSerializer.Deserialize<PatrolScriptComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "scriptlua" => JsonSerializer.Deserialize<LuaScriptComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "audiosource" => JsonSerializer.Deserialize<AudioSourceComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "navagent" => JsonSerializer.Deserialize<NavAgentComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "prefabinstance" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.PrefabInstanceComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "particleemitter" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.ParticleEmitterComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "crossscenereference" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.CrossSceneReferenceComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "collider" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.ColliderComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "rigidbody" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.RigidBodyComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "particlefx" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.ParticleFXComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            "gameplayrules" => JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.GameplayRulesComponent>(componentDoc.Data.GetRawText(), SceneDocument.JsonOptions),
            _ => throw new NotSupportedException($"Unknown component type discriminator: '{componentDoc.Type}'")
        };
    }

    private static void ValidateAndBuildHierarchy(SceneDocument document, Dictionary<EntityGuid, GameEntity> entityMap)
    {
        var visited = new HashSet<EntityGuid>();
        var recursionStack = new HashSet<EntityGuid>();

        foreach (var entityDoc in document.Entities)
        {
            if (entityDoc.ParentEntityGuid is { } parentGuid)
            {
                if (!entityMap.TryGetValue(parentGuid, out var parentEntity))
                {
                    throw new InvalidOperationException($"Parent EntityGuid '{parentGuid}' not found for entity '{entityDoc.EntityGuid}'");
                }

                if (entityMap.TryGetValue(entityDoc.EntityGuid, out var childEntity))
                {
                    childEntity.Parent = parentEntity;
                    parentEntity.Children.Add(childEntity);

                    if (!visited.Contains(entityDoc.EntityGuid))
                    {
                        if (HasCycle(entityDoc.EntityGuid, document, entityMap, visited, recursionStack))
                        {
                            throw new InvalidOperationException($"Cycle detected in entity hierarchy starting from EntityGuid '{entityDoc.EntityGuid}'");
                        }
                    }
                }
            }
        }
    }

    private static bool HasCycle(EntityGuid current, SceneDocument document, Dictionary<EntityGuid, GameEntity> entityMap, HashSet<EntityGuid> visited, HashSet<EntityGuid> recursionStack)
    {
        visited.Add(current);
        recursionStack.Add(current);

        var entityDoc = document.Entities.FirstOrDefault(e => e.EntityGuid == current);
        if (entityDoc != null && entityDoc.ParentEntityGuid is { } parentGuid)
        {
            if (recursionStack.Contains(parentGuid))
            {
                return true;
            }

            if (!visited.Contains(parentGuid))
            {
                if (HasCycle(parentGuid, document, entityMap, visited, recursionStack))
                {
                    return true;
                }
            }
        }

        recursionStack.Remove(current);
        return false;
    }

    public static void SaveScene(Scene scene, string scenePath)
    {
        ArgumentNullException.ThrowIfNull(scene);
        var document = ToSceneDocument(scene);
        SaveSceneDocument(document, scenePath);
    }

    internal static void SaveSceneDocument(SceneDocument document, string scenePath)
    {
        ArgumentNullException.ThrowIfNull(document);
        var partialPath = scenePath + ".partial";
        if (document.SceneGuid.Value == Guid.Empty)
        {
            throw new InvalidDataException("Scene Guid cannot be empty.");
        }

        try
        {
            var directory = Path.GetDirectoryName(scenePath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var json = JsonSerializer.Serialize(document, SceneDocument.JsonOptions) + Environment.NewLine;
            using (var stream = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(json);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }

            var validation = JsonSerializer.Deserialize<SceneDocument>(File.ReadAllText(partialPath), SceneDocument.JsonOptions);
            if (validation is not { IsValid: true }) throw new InvalidDataException("Scene document validation failed.");
            File.Move(partialPath, scenePath, overwrite: true);
            Logger.Info($"Scene saved: {scenePath}");
        }
        catch
        {
            try { if (File.Exists(partialPath)) File.Delete(partialPath); } catch { }
            throw;
        }
    }

    public static Scene? LoadScene(string scenePath)
    {
        var partialPath = scenePath + ".partial";
        if (!File.Exists(scenePath))
        {
            try { if (File.Exists(partialPath)) File.Delete(partialPath); } catch { }
            Logger.Error($"Scene file not found: {scenePath}");
            return null;
        }

        try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(scenePath));
            var document = json.RootElement.Deserialize<SceneDocument>(SceneDocument.JsonOptions);
            if (document is not { IsValid: true })
                throw new InvalidDataException($"Scene document is invalid: {scenePath}");
            return FromSceneDocument(document);
        }
        catch (Exception ex) when (ex is JsonException or NotSupportedException or InvalidOperationException)
        {
            throw new InvalidDataException($"Failed to load scene '{scenePath}': {ex.Message}", ex);
        }
    }
}
