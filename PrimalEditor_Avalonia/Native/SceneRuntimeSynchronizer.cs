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
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Native;

public sealed class SceneRuntimeSynchronizer
{
    /// <summary>
    /// Raised when an asset editor persists new content for a material
    /// asset (sidecar rewritten on disk). The owning world editor
    /// subscribes so entities that already reference the material pick up
    /// the change without an app restart. Static because asset editors
    /// are free-floating windows with no path back to their
    /// <see cref="ViewModels.WorldEditorViewModel"/>.
    /// </summary>
    public static event Action<AssetGuid>? MaterialSavedOnDisk;

    /// <summary>Publish a "material file was re-saved on disk" notification.</summary>
    public static void NotifyMaterialSaved(AssetGuid guid) => MaterialSavedOnDisk?.Invoke(guid);

    private readonly INativeEngineBridgeLike _bridge;
    private readonly RuntimeEntityMap _entities;
    private readonly RuntimeAssetMap _assets;
    private readonly RuntimeMaterialMap _materials;
    private readonly List<GameEntity> _live = new();
    private readonly Dictionary<AssetGuid, uint> _materialGraphIds = new();
    private readonly Dictionary<string, ulong> _worldTextureIds = new(StringComparer.OrdinalIgnoreCase);
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
        foreach (var graphId in _materialGraphIds.Values)
        {
            if (graphId == 0) continue;
            try { MaterialGraphNative.Destroy(graphId); }
            catch { /* engine might be torn down already */ }
        }
        _materialGraphIds.Clear();
        _worldTextureIds.Clear();
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

	/// <summary>
	/// Remove one managed entity and its descendants from the live native
	/// scene. The production bridge also unregisters the pipeline mesh that
	/// belongs to each source entity.
	/// </summary>
	public void RemoveEntity(GameEntity entity)
	{
		ArgumentNullException.ThrowIfNull(entity);
		foreach (var child in entity.Children.ToArray()) RemoveEntity(child);

		if (_entities.GetEntityId(entity.EntityGuid) is { } runtimeId)
		{
			_bridge.DestroyEntity(runtimeId);
			_entities.Unregister(entity.EntityGuid);
		}
		_live.Remove(entity);
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
				if (contentId != NativeEngineBridge.InvalidContentId)
					_assets.Register(meshRenderer.MeshGuid, contentId);
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

            // Keep the editor-side material runtime map populated for
            // material lifecycle/state tracking. Forward rendering binds the
            // resolved texture content IDs below; these runtime IDs must not
            // be passed as texture IDs.
            foreach (var materialGuid in meshRenderer.MaterialGuids)
                EnsureMaterial(materialGuid);

            var textureIds = ResolveWorldTextureIds(meshRenderer);
            _bridge.SetEntityMeshTextures(runtimeId, finalMeshContentId.Value, textureIds);

            // Apply per-material node graphs (Unreal-style material editor).
            // We push graphs by material-GUID → sidecar resolution so the
            // engine re-evaluates each graph with its own topology.
            if (meshRenderer.MaterialGuids.Count > 0)
            {
                for (int i = 0; i < meshRenderer.MaterialGuids.Count; i++)
                {
                    var graphId = EnsureMaterialGraph(meshRenderer.MaterialGuids[i]);
                    if (graphId != 0)
                        _bridge.ApplyMaterialGraph(runtimeId, graphId);
                }
            }
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

    /// <summary>
    /// If <paramref name="materialGuid"/> resolves to a material file with
    /// a sibling <c>.material.graph.json</c> sidecar, load it into a fresh
    /// native graph and remember the handle. Returns the cached handle on
    /// repeat calls; returns 0 when no sidecar is present or the engine
    /// isn't loaded.
    /// </summary>
    private uint EnsureMaterialGraph(AssetGuid materialGuid)
    {
        if (materialGuid.Value == Guid.Empty) return 0;
        if (!_bridge.IsLoaded) return 0;

        if (_materialGraphIds.TryGetValue(materialGuid, out var cached))
            return cached;

        var info = AssetRegistry.GetByGuid(materialGuid.Value);
        if (info is null) return 0;

        var sidecar = Path.ChangeExtension(info.FullPath, ".material.graph.json");
        if (!File.Exists(sidecar)) return 0;

        var graphId = MaterialGraphLoaderHook?.Invoke(sidecar) ?? MaterialGraphNative.LoadFromFile(sidecar);
        if (graphId == 0) return 0;

        _materialGraphIds[materialGuid] = graphId;
        return graphId;
    }

    /// <summary>
    /// Test seam over <see cref="MaterialGraphNative.LoadFromFile"/> so
    /// graph-cache invalidation can be exercised without the engine dylib.
    /// Null in production; only tests assign it.
    /// </summary>
    internal Func<string, uint>? MaterialGraphLoaderHook { get; set; }

    /// <summary>
    /// Drop the cached native graph for an already-referenced material and
    /// re-apply it to every live entity that uses it. Called after the
    /// material editor rewrites the sidecar so open scenes render the new
    /// graph without the app restart a full <see cref="Sync"/> used to
    /// require. Eviction must happen before any reload:
    /// <see cref="EnsureMaterialGraph"/> would otherwise hand back the
    /// pre-save topology still held by the cached native handle.
    /// </summary>
    public void ReloadMaterial(AssetGuid materialGuid)
    {
        if (materialGuid.Value == Guid.Empty) return;
        if (!_bridge.IsLoaded) return;

        if (_materialGraphIds.Remove(materialGuid, out var stale) && stale != 0)
        {
            try { MaterialGraphNative.Destroy(stale); }
            catch { /* engine might be torn down already */ }
        }

        foreach (var entity in _live)
        {
            var renderer = entity.GetComponent<MeshRendererComponent>();
            if (renderer == null || !renderer.MaterialGuids.Contains(materialGuid)) continue;
            RefreshEntityMaterials(entity);
        }
    }

    private ulong[] ResolveWorldTextureIds(MeshRendererComponent renderer)
    {
        if (renderer.MaterialGuids.Count == 0) return Array.Empty<ulong>();
        var material = AssetRegistry.GetByGuid(renderer.MaterialGuids[0].Value);
        if (material == null) return Array.Empty<ulong>();

        var paths = MaterialWorldTextureResolver.Resolve(material.FullPath);
        var last = -1;
        for (var i = 0; i < paths.Length; i++)
            if (!string.IsNullOrWhiteSpace(paths[i])) last = i;
        if (last < 0) return Array.Empty<ulong>();

        var ids = new ulong[last + 1];
        Array.Fill(ids, NativeEngineBridge.InvalidContentId);
        for (var i = 0; i <= last; i++)
        {
            var path = paths[i];
            if (string.IsNullOrWhiteSpace(path)) continue;
            if (!_worldTextureIds.TryGetValue(path, out var id))
            {
                id = _bridge.ImportCookedTexture(path);
                if (id != NativeEngineBridge.InvalidContentId)
                {
                    _worldTextureIds[path] = id;
                    Logger.Info($"SceneRuntimeSynchronizer: bound world texture '{path}' (content={id})");
                }
                else
                {
                    Logger.Warning($"SceneRuntimeSynchronizer: failed to import world texture '{path}'");
                }
            }
            ids[i] = id;
        }
        return ids;
    }

    private string? ResolveCookedArtifactPath(AssetInfo info)
    {
        var relative = AssetRegistry.GetRelativePath(info.FullPath);
        if (relative is not { } rel) return UseSourceAsFallback ? info.FullPath : null;

        var projectRoot = AssetRegistry.ProjectRoot;
        if (string.IsNullOrEmpty(projectRoot)) return UseSourceAsFallback ? info.FullPath : null;

		var artifactRelativePath = rel.Value;
		string? sourceArtifactPath = info.FullPath;
		const string modelSidecarSuffix = ".model.json";
		if (artifactRelativePath.EndsWith(modelSidecarSuffix, StringComparison.OrdinalIgnoreCase))
		{
			artifactRelativePath = artifactRelativePath[..^modelSidecarSuffix.Length] + ".mesh";
			sourceArtifactPath = info.FullPath[..^modelSidecarSuffix.Length] + ".mesh";
		}
		else if (Path.GetExtension(artifactRelativePath).Equals(".model", StringComparison.OrdinalIgnoreCase))
		{
			artifactRelativePath = Path.ChangeExtension(artifactRelativePath, ".mesh");
			sourceArtifactPath = Path.ChangeExtension(info.FullPath, ".mesh");
		}

        var cookedAbsolute = Path.Combine(
            projectRoot, ".Primal", "Cooked", CookedPlatform, CookedConfiguration,
			artifactRelativePath.Replace('/', Path.DirectorySeparatorChar));

        if (File.Exists(cookedAbsolute)) return cookedAbsolute;
		if (UseSourceAsFallback && File.Exists(sourceArtifactPath)) return sourceArtifactPath;
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
            : entity.GetComponent<LightComponent>() != null || entity.GetComponent<EnvironmentComponent>() != null
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
    /// Rebind the selected mesh's persisted material slots without rebuilding
    /// the scene. The production bridge replaces the pipeline render entity,
    /// so its transform and material graph are restored immediately.
    /// </summary>
    public void RefreshEntityMaterials(GameEntity entity)
    {
        ArgumentNullException.ThrowIfNull(entity);
        if (!_bridge.IsLoaded) return;
        if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) return;
        var renderer = entity.GetComponent<MeshRendererComponent>();
        if (renderer == null) return;

        ulong geometryId;
        if (_assets.GetContentId(renderer.MeshGuid) is { } imported
            && imported != NativeEngineBridge.InvalidContentId)
        {
            geometryId = imported;
        }
        else if (renderer.MeshGuid.Value == Guid.Empty
                 && renderer.BuiltinMesh != BuiltinMeshKind.None)
        {
            geometryId = EnsureBuiltinGeometry(renderer.BuiltinMesh);
        }
        else
        {
            return;
        }
        if (geometryId == NativeEngineBridge.InvalidContentId) return;

        var textureIds = ResolveWorldTextureIds(renderer);
        _bridge.SetEntityMeshTextures(runtimeId, geometryId, textureIds);

        var transform = entity.GetTransform();
        if (transform != null)
        {
            _bridge.SetEntityTransform(runtimeId,
                new[] { transform.PositionX, transform.PositionY, transform.PositionZ },
                new[] { transform.RotationX, transform.RotationY, transform.RotationZ },
                new[] { transform.ScaleX, transform.ScaleY, transform.ScaleZ });
        }

        var appliedAnyGraph = false;
        foreach (var materialGuid in renderer.MaterialGuids)
        {
            var graphId = EnsureMaterialGraph(materialGuid);
            if (graphId != 0)
            {
                _bridge.ApplyMaterialGraph(runtimeId, graphId);
                appliedAnyGraph = true;
            }
        }
        // All slots lost their graphs (e.g. the inspector swapped to a
        // material without a sidecar) — drop a previously compiled variant so
        // the entity falls back to the fixed material path immediately.
        if (!appliedAnyGraph)
        {
            try { _bridge.ClearEntityMaterialGraph(runtimeId); }
            catch (NotSupportedException) { /* older bridge without graph support */ }
        }
    }

    /// <summary>Import and rebind geometry after the Inspector replaces a Mesh asset.</summary>
    public void RefreshEntityMesh(GameEntity entity)
    {
        ArgumentNullException.ThrowIfNull(entity);
        if (!_bridge.IsLoaded) return;
        if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) return;
        var renderer = entity.GetComponent<MeshRendererComponent>();
        if (renderer == null) return;

        if (renderer.MeshGuid.Value == Guid.Empty && renderer.BuiltinMesh == BuiltinMeshKind.None)
        {
            _bridge.UnbindEntityMesh(runtimeId);
            return;
        }

        if (renderer.MeshGuid.Value != Guid.Empty && _assets.GetContentId(renderer.MeshGuid) is null)
        {
            var info = AssetRegistry.GetByGuid(renderer.MeshGuid.Value);
            var path = info == null ? null : ResolveCookedArtifactPath(info);
            if (string.IsNullOrWhiteSpace(path))
            {
                Logger.Warning($"SceneRuntimeSynchronizer: cannot resolve replacement mesh {renderer.MeshGuid}");
                return;
            }
            var contentId = _bridge.ImportMesh(path);
            if (contentId == NativeEngineBridge.InvalidContentId) return;
            _assets.Register(renderer.MeshGuid, contentId);
        }

        RefreshEntityMaterials(entity);
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
					if (contentId != NativeEngineBridge.InvalidContentId)
						_assets.Register(meshRenderer.MeshGuid, contentId);
                }
            }
        }

        if (_entities.GetEntityId(entity.EntityGuid) is { } runtimeId)
        {
            RefreshEntityMaterials(entity);

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
		if (guid.Value == Guid.Empty) return;
        if (contentId == NativeEngineBridge.InvalidContentId) return;
        _assets.Register(guid, contentId);
    }
}
