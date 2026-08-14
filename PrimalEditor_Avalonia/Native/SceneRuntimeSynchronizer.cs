using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Native;

public sealed class SceneRuntimeSynchronizer
{
    private readonly INativeEngineBridgeLike _bridge;
    private readonly RuntimeEntityMap _entities;
    private readonly RuntimeAssetMap _assets;
    private readonly RuntimeMaterialMap _materials;
    private readonly List<GameEntity> _live = new();
    private ulong _defaultMaterialId;
    private readonly Dictionary<BuiltinMeshKind, ulong> _builtinGeometryIds = new();
    private readonly Dictionary<EditorHelperKind, ulong> _editorHelperGeometryIds = new();

    private enum EditorHelperKind
    {
        Light,
        Camera,
    }

    public SceneRuntimeSynchronizer(
        INativeEngineBridgeLike bridge,
        RuntimeEntityMap entities,
        RuntimeAssetMap assets,
        RuntimeMaterialMap materials)
    {
        _bridge = bridge;
        _entities = entities;
        _assets = assets;
        _materials = materials;
    }

    /// <summary>Target platform whose Cooked directory the synchronizer should resolve to. Defaults to "Editor".</summary>
    public string CookedPlatform { get; set; } = "Editor";
    /// <summary>Build configuration whose Cooked directory the synchronizer should resolve to. Defaults to "DebugEditor".</summary>
    public string CookedConfiguration { get; set; } = "DebugEditor";
    /// <summary>
    /// Set true to fall back to the source asset path when no cooked
    /// artifact exists yet. Useful for editor sessions where the user
    /// is iterating on assets without rebuilding the Cooked folder
    /// each time; production builds should set this false.
    /// </summary>
    public bool UseSourceAsFallback { get; set; } = true;

    public bool IsBridgeAvailable => _bridge.IsLoaded;

    /// <summary>
    /// Pushes editor-side transform changes through the same bridge instance
    /// that registered the entity's render resources. The production adapter
    /// mirrors this update to the separate pipeline mesh entity created by
    /// PipelineRegisterMeshEntity; calling NativeEngineBridge directly would
    /// update only the source ECS entity and leave the visible mesh at its old
    /// transform.
    /// </summary>
    public void PushTransforms(IEnumerable<GameEntity> entities)
    {
        if (!_bridge.IsLoaded) return;
        foreach (var entity in entities)
        {
            var transform = entity.GetTransform();
            if (transform is null) continue;
            if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) continue;

            _bridge.SetEntityTransform(runtimeId,
                new[] { transform.PositionX, transform.PositionY, transform.PositionZ },
                new[] { transform.RotationX, transform.RotationY, transform.RotationZ },
                new[] { transform.ScaleX, transform.ScaleY, transform.ScaleZ });
        }
    }

    public void Teardown()
    {
        foreach (var entity in _live)
        {
            var id = _entities.GetEntityId(entity.EntityGuid);
            if (id is { } runtimeId) _bridge.DestroyEntity(runtimeId);
            _entities.Unregister(entity.EntityGuid);
        }
        _live.Clear();
        _assets.Clear();
        _materials.Clear();
        _defaultMaterialId = 0;
        foreach (var geometryId in _builtinGeometryIds.Values)
        {
            if (geometryId == NativeEngineBridge.InvalidContentId) continue;
            try { _bridge.ProceduralMeshDestroy(geometryId); }
            catch { /* ignore: engine might be torn down already */ }
        }
        _builtinGeometryIds.Clear();
        foreach (var geometryId in _editorHelperGeometryIds.Values)
        {
            if (geometryId == NativeEngineBridge.InvalidContentId) continue;
            try { _bridge.ProceduralMeshDestroy(geometryId); }
            catch { /* ignore: engine might be torn down already */ }
        }
        _editorHelperGeometryIds.Clear();
    }

    public void Sync(Scene scene)
    {
        ArgumentNullException.ThrowIfNull(scene);
        Teardown();
        if (!_bridge.IsLoaded) return;

        // Pre-create the editor's default white PBR material once per
        // scene activation. Entities whose MeshRendererComponent has
        // an empty MeshGuid (Character placeholder, future "use
        // default" props) bind this material; the procedural sphere
        // geometry below stands in for the missing mesh. Caching
        // avoids leaking a material id per toolbar click.
        _defaultMaterialId = _bridge.CreateDefaultMaterial();

        foreach (var entity in scene.Entities)
        {
            if (entity.EntityGuid.Value == Guid.Empty) continue;
            var position = entity.GetTransform();
            var entityId = _bridge.CreateEntity(
                position?.PositionX ?? 0f,
                position?.PositionY ?? 0f,
                position?.PositionZ ?? 0f);
            if (entityId == 0 || entityId == NativeEngineBridge.InvalidEntityId) continue;
            _entities.Register(entity.EntityGuid, entityId);
            _live.Add(entity);
        }

        foreach (var entity in _live)
        {
            var meshRenderer = entity.GetComponent<MeshRendererComponent>();
            if (meshRenderer is null) continue;
            if (meshRenderer.MeshGuid.Value != Guid.Empty &&
                _assets.GetContentId(meshRenderer.MeshGuid) is null)
            {
                // Mesh GUID → AssetInfo (AssetRegistry) → ProjectRelativePath
                // → absolute cooked artifact path → ImportMesh. The .meta
                // file is the authority for the GUID; ProjectRelativePath
                // is the authority for the on-disk location; the cooked
                // artifact is what the engine actually loads.
                //
                // Fallback: if AssetRegistry isn't initialised yet (the
                // // legacy tests + early integration phases still call
                // // Sync before Initialize), pass the GUID string the
                // // engine used to expect. New code should always have
                // // the AssetRegistry up before Sync is called.
                var info = AssetRegistry.GetByGuid(meshRenderer.MeshGuid.Value);
                string? cookedPath = null;
                if (info != null)
                {
                    cookedPath = ResolveCookedArtifactPath(info);
                    if (string.IsNullOrEmpty(cookedPath))
                    {
                        Logger.Warning($"SceneRuntimeSynchronizer: cooked artifact missing for {info.FullPath}");
                        continue;
                    }
                }
                else if (UseSourceAsFallback)
                {
                    // Lazy registration: register on-demand so the test
                    // path that ships a guid-without-asset-registration
                    // still walks the bridge.
                    cookedPath = meshRenderer.MeshGuid.Value.ToString("D");
                }
                else
                {
                    Logger.Warning($"SceneRuntimeSynchronizer: mesh asset not registered (guid={meshRenderer.MeshGuid})");
                    continue;
                }
                var contentId = _bridge.ImportMesh(cookedPath!);
                if (contentId != 0) _assets.Register(meshRenderer.MeshGuid, contentId);
            }

            if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) continue;
            var meshContentId = _assets.GetContentId(meshRenderer.MeshGuid);
            ulong? finalMeshContentId = null;
            if (meshContentId.HasValue && meshContentId.Value != NativeEngineBridge.InvalidContentId)
            {
                finalMeshContentId = meshContentId.Value;
            }
            else if (meshRenderer.MeshGuid.Value == Guid.Empty &&
                     meshRenderer.BuiltinMesh != BuiltinMeshKind.None)
            {
                // Built-in toolbar entities intentionally have no asset
                // GUID; resolve their persisted procedural mesh kind.
                finalMeshContentId = EnsureBuiltinGeometry(meshRenderer.BuiltinMesh);
            }
            if (!finalMeshContentId.HasValue ||
                finalMeshContentId.Value == NativeEngineBridge.InvalidContentId) continue;

            var materialIds = meshRenderer.MaterialGuids
                .Select(EnsureMaterial)
                .Where(id => id != 0)
                .ToArray();
            if (materialIds.Length == 0 && _defaultMaterialId != 0)
            {
                materialIds = new uint[] { (uint)_defaultMaterialId };
            }
            _bridge.SetEntityMesh(runtimeId, finalMeshContentId.Value, materialIds, (uint)materialIds.Length);
        }

        foreach (var entity in _live)
        {
            if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) continue;

            foreach (var light in entity.Components.OfType<LightComponent>())
            {
                _bridge.AddEntityLight(runtimeId,
                    (uint)light.LightType,
                    light.Intensity,
                    new[] { light.ColorR, light.ColorG, light.ColorB },
                    light.Range,
                    light.UmbraAngle,
                    light.PenumbraAngle,
                    light.IsEnabled);
            }

            foreach (var camera in entity.Components.OfType<CameraComponent>())
            {
                _bridge.AddEntityCamera(runtimeId,
                    (uint)camera.Projection,
                    // Graphics backends store FOV as a fraction of PI.
                    camera.FieldOfViewDegrees / 180f,
                    aspectRatio: 1.0f,
                    nearZ: camera.NearPlane,
                    farZ: camera.FarPlane,
                    upVector: new[] { 0f, 1f, 0f });
            }

            BindEditorHelperMesh(entity, runtimeId);

            var transform = entity.GetTransform();
            if (transform is null) continue;
            _bridge.SetEntityTransform(runtimeId,
                new[] { transform.PositionX, transform.PositionY, transform.PositionZ },
                new[] { transform.RotationX, transform.RotationY, transform.RotationZ },
                new[] { transform.ScaleX, transform.ScaleY, transform.ScaleZ });
        }
    }

    private uint EnsureMaterial(AssetGuid guid)
    {
        if (guid.Value == Guid.Empty) return 0;
        var existing = _materials.GetRuntimeId(guid);
        if (existing is { } already) return already;
        if (!_bridge.IsLoaded) return 0;
        try
        {
            var runtime = _bridge.CreateMaterial(guid);
            if (runtime != 0) _materials.Register(guid, runtime);
            return runtime;
        }
        catch (NotSupportedException)
        {
            return 0;
        }
    }

    private string? ResolveCookedArtifactPath(AssetInfo info)
    {
        var relative = AssetRegistry.GetRelativePath(info.FullPath);
        if (relative is not { } rel) return UseSourceAsFallback ? info.FullPath : null;

        var projectRoot = AssetRegistry.ProjectRoot;
        if (string.IsNullOrEmpty(projectRoot)) return UseSourceAsFallback ? info.FullPath : null;

        var cookedAbsolute = Path.Combine(
            projectRoot, ".Primal", "Cooked", CookedPlatform, CookedConfiguration,
            rel.Value.Replace('/', Path.DirectorySeparatorChar));

        if (File.Exists(cookedAbsolute)) return cookedAbsolute;
        if (UseSourceAsFallback) return info.FullPath;
        return null;
    }

    /// <summary>
    /// Lazily create and cache one native procedural geometry resource per
    /// built-in kind. These resources live at the RHI content system level
    /// (not on disk) and are recreated on every
    /// <see cref="Sync"/> / scene activation.
    /// </summary>
    private ulong EnsureBuiltinGeometry(BuiltinMeshKind kind)
    {
        if (kind == BuiltinMeshKind.None) return 0;
        if (_builtinGeometryIds.TryGetValue(kind, out var existing) &&
            existing != NativeEngineBridge.InvalidContentId)
            return existing;

        var (meshType, parameters) = kind switch
        {
            BuiltinMeshKind.Sphere => (0u, new float[] { 0.5f, 24f, 16f }),
            BuiltinMeshKind.Box => (3u, new float[] { 1f, 1f, 1f }),
            BuiltinMeshKind.Plane => (9u, new float[] { 2f, 2f, 1f, 1f }),
            BuiltinMeshKind.Capsule => (5u, new float[] { 0.5f, 1f, 20f, 8f }),
            _ => (uint.MaxValue, System.Array.Empty<float>()),
        };
        if (meshType == uint.MaxValue) return 0;

        var geometryId = _bridge.ProceduralMeshCreate(meshType, parameters);
        _builtinGeometryIds[kind] = geometryId;
        return geometryId;
    }

    /// <summary>
    /// Light and Camera entities need a dependable native-surface glyph.
    /// The managed overlay sits behind NSView-backed Metal surfaces on macOS,
    /// so a small editor-only mesh is registered in addition to the wire icon.
    /// It is mapped to the source runtime entity by the bridge and therefore
    /// receives every subsequent position/rotation/scale update.
    /// </summary>
    private void BindEditorHelperMesh(GameEntity entity, uint runtimeId)
    {
        // Do not replace a real renderer attached to a multi-component entity.
        if (entity.GetComponent<MeshRendererComponent>() != null) return;

        EditorHelperKind? kind = entity.GetComponent<CameraComponent>() != null
            ? EditorHelperKind.Camera
            : entity.GetComponent<LightComponent>() != null
                ? EditorHelperKind.Light
                : null;
        if (!kind.HasValue) return;

        if (!_editorHelperGeometryIds.TryGetValue(kind.Value, out var geometryId) ||
            geometryId == NativeEngineBridge.InvalidContentId)
        {
            // Light: compact sphere glyph. Camera: a rectangular camera body.
            var (meshType, parameters) = kind.Value switch
            {
                EditorHelperKind.Light => (0u, new float[] { 0.36f, 16f, 12f }),
                EditorHelperKind.Camera => (3u, new float[] { 0.62f, 0.42f, 0.78f }),
                _ => (uint.MaxValue, System.Array.Empty<float>()),
            };
            if (meshType == uint.MaxValue) return;
            geometryId = _bridge.ProceduralMeshCreate(meshType, parameters);
            _editorHelperGeometryIds[kind.Value] = geometryId;
        }

        if (geometryId == NativeEngineBridge.InvalidContentId) return;
        var materials = _defaultMaterialId != 0
            ? new uint[] { (uint)_defaultMaterialId }
            : System.Array.Empty<uint>();
        _bridge.SetEntityMesh(runtimeId, geometryId, materials, (uint)materials.Length);
    }

    /// <summary>
    /// Push a single entity (created via the toolbar / drag-drop) without
    /// re-syncing the rest of the scene. Cheap enough for the "add one
    /// entity" interactive flow; full Sync() is still used on scene
    /// activation.
    /// </summary>
    public void SyncEntity(GameEntity entity)
    {
        ArgumentNullException.ThrowIfNull(entity);
        if (!_bridge.IsLoaded) return;
        if (entity.EntityGuid.Value == Guid.Empty) return;
        if (_entities.GetEntityId(entity.EntityGuid) is not null) return;

        if (_defaultMaterialId == 0)
        {
            _defaultMaterialId = _bridge.CreateDefaultMaterial();
        }

        var position = entity.GetTransform();
        var entityId = _bridge.CreateEntity(
            position?.PositionX ?? 0f,
            position?.PositionY ?? 0f,
            position?.PositionZ ?? 0f);
        if (entityId == 0 || entityId == NativeEngineBridge.InvalidEntityId) return;
        _entities.Register(entity.EntityGuid, entityId);
        if (!_live.Contains(entity)) _live.Add(entity);

        var meshRenderer = entity.GetComponent<MeshRendererComponent>();
        if (meshRenderer != null
            && meshRenderer.MeshGuid.Value != Guid.Empty
            && _assets.GetContentId(meshRenderer.MeshGuid) is null)
        {
            var info = AssetRegistry.GetByGuid(meshRenderer.MeshGuid.Value);
            if (info != null)
            {
                var cookedPath = ResolveCookedArtifactPath(info);
                if (!string.IsNullOrEmpty(cookedPath))
                {
                    var contentId = _bridge.ImportMesh(cookedPath!);
                    if (contentId != 0) _assets.Register(meshRenderer.MeshGuid, contentId);
                }
            }
        }

        if (_entities.GetEntityId(entity.EntityGuid) is { } runtimeId)
        {
            if (meshRenderer != null
                && _assets.GetContentId(meshRenderer.MeshGuid) is { } meshContentId
                && meshContentId != NativeEngineBridge.InvalidContentId)
            {
                var materials = _defaultMaterialId != 0
                    ? new uint[] { (uint)_defaultMaterialId }
                    : System.Array.Empty<uint>();
                _bridge.SetEntityMesh(runtimeId, meshContentId, materials, (uint)materials.Length);
            }
            else if (meshRenderer != null
                && meshRenderer.MeshGuid.Value == Guid.Empty
                && meshRenderer.BuiltinMesh != BuiltinMeshKind.None)
            {
                // A persisted built-in kind takes the procedural path when
                // no disk-backed mesh asset is assigned.
                var geometryId = EnsureBuiltinGeometry(meshRenderer.BuiltinMesh);
                if (geometryId != NativeEngineBridge.InvalidContentId)
                {
                    var materials = _defaultMaterialId != 0
                        ? new uint[] { (uint)_defaultMaterialId }
                        : System.Array.Empty<uint>();
                    _bridge.SetEntityMesh(runtimeId, geometryId, materials, (uint)materials.Length);
                }
            }

            foreach (var light in entity.Components.OfType<LightComponent>())
            {
                _bridge.AddEntityLight(runtimeId,
                    (uint)light.LightType,
                    light.Intensity,
                    new[] { light.ColorR, light.ColorG, light.ColorB },
                    light.Range,
                    light.UmbraAngle,
                    light.PenumbraAngle,
                    light.IsEnabled);
            }

            foreach (var camera in entity.Components.OfType<CameraComponent>())
            {
                _bridge.AddEntityCamera(runtimeId,
                    (uint)camera.Projection,
                    camera.FieldOfViewDegrees / 180f,
                    aspectRatio: 1.0f,
                    nearZ: camera.NearPlane,
                    farZ: camera.FarPlane,
                    upVector: new[] { 0f, 1f, 0f });
            }

            BindEditorHelperMesh(entity, runtimeId);

            if (position != null)
            {
                _bridge.SetEntityTransform(runtimeId,
                    new[] { position.PositionX, position.PositionY, position.PositionZ },
                    new[] { position.RotationX, position.RotationY, position.RotationZ },
                    new[] { position.ScaleX, position.ScaleY, position.ScaleZ });
            }
        }
    }

    /// <summary>
    /// Register a content id against an AssetGuid without re-importing.
    /// Used by the drag-drop / "Add Prop" flows which already drove
    /// the async ImportMesh themselves and want to avoid paying the
    /// import cost twice when <see cref="SyncEntity"/> runs.
    /// </summary>
    public void RegisterPreImportedMesh(AssetGuid guid, ulong contentId)
    {
        if (guid.Value == Guid.Empty || contentId == 0) return;
        if (contentId == NativeEngineBridge.InvalidContentId) return;
        _assets.Register(guid, contentId);
    }
}
