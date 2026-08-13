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
    private ulong _builtinSphereGeometryId;

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
        if (_builtinSphereGeometryId != 0 && _builtinSphereGeometryId != NativeEngineBridge.InvalidContentId)
        {
            try { _bridge.ProceduralMeshDestroy(_builtinSphereGeometryId); }
            catch { /* ignore: engine might be torn down already */ }
        }
        _builtinSphereGeometryId = 0;
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
            ulong finalMeshContentId = 0;
            if (meshContentId.HasValue && meshContentId.Value != NativeEngineBridge.InvalidContentId)
            {
                finalMeshContentId = meshContentId.Value;
            }
            else if (meshRenderer.MeshGuid.Value == Guid.Empty)
            {
                // Toolbar-built entities (Character, etc.) ship with an
                // empty MeshGuid — use the built-in placeholder sphere so
                // they render with the default white PBR material.
                // The user can later drag a real .mesh onto the entity
                // and SyncEntity will pick up the new guid on the next
                // call.
                var sphereGeom = EnsureBuiltinSphereGeometry();
                if (sphereGeom != 0 && sphereGeom != NativeEngineBridge.InvalidContentId)
                    finalMeshContentId = sphereGeom;
            }
            if (finalMeshContentId == 0) continue;

            var materialIds = meshRenderer.MaterialGuids
                .Select(EnsureMaterial)
                .Where(id => id != 0)
                .ToArray();
            if (materialIds.Length == 0 && _defaultMaterialId != 0)
            {
                materialIds = new uint[] { (uint)_defaultMaterialId };
            }
            _bridge.SetEntityMesh(runtimeId, finalMeshContentId, materialIds, (uint)materialIds.Length);
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
                    camera.FieldOfViewDegrees * (MathF.PI / 180f),
                    aspectRatio: 1.0f,
                    nearZ: camera.NearPlane,
                    farZ: camera.FarPlane,
                    upVector: new[] { 0f, 1f, 0f });
            }

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
    /// Lazily cook a small unit sphere on the native side. The id is
    /// cached and reused for every entity whose MeshGuid equals
    /// <c>Guid.Empty</c>. The procedural mesh lives at the RHI content
    /// system level (not on disk) — it's recreated on every
    /// <see cref="Sync"/> / scene activation.
    /// </summary>
    private ulong EnsureBuiltinSphereGeometry()
    {
        if (_builtinSphereGeometryId != 0 && _builtinSphereGeometryId != NativeEngineBridge.InvalidContentId)
            return _builtinSphereGeometryId;
        const uint kSphereType = 0;
        var sphereParams = new float[] { 0.5f, 16, 12 };
        _builtinSphereGeometryId = _bridge.ProceduralMeshCreate(kSphereType, sphereParams);
        return _builtinSphereGeometryId;
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
                && meshRenderer.MeshGuid.Value == Guid.Empty)
            {
                // Empty MeshGuid on a mesh renderer = "use the built-in
                // placeholder sphere". This is the contract the toolbar
                // uses for Character entities; the user can later drag a
                // real .mesh onto the entity and SyncEntity will pick up
                // the new guid on the next call.
                var sphereGeom = EnsureBuiltinSphereGeometry();
                if (sphereGeom != 0 && sphereGeom != NativeEngineBridge.InvalidContentId)
                {
                    var materials = _defaultMaterialId != 0
                        ? new uint[] { (uint)_defaultMaterialId }
                        : System.Array.Empty<uint>();
                    _bridge.SetEntityMesh(runtimeId, sphereGeom, materials, (uint)materials.Length);
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
                    camera.FieldOfViewDegrees,
                    aspectRatio: 1.0f,
                    nearZ: camera.NearPlane,
                    farZ: camera.FarPlane,
                    upVector: new[] { 0f, 1f, 0f });
            }

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
