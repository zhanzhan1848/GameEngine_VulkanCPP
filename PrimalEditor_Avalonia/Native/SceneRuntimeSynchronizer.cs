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
    // scripting S1：实体 GUID → 引擎脚本 handle（Teardown/参数热改时销毁重建）。
    private readonly Dictionary<Guid, ulong> _scriptHandles = new();

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

            // PFX 发射器跟随实体位置（拖拽 gizmo / 属性面板改位置即生效）
            if (entity.GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>() != null
                && PfxEmitterByRuntime.TryGetValue(runtimeId, out var pfxId))
            {
                _bridge.PfxSetEmitterPosition(pfxId,
                    transform.PositionX, transform.PositionY, transform.PositionZ);
            }
        }
    }

    public void Teardown()
    {
        // 脚本先行拆除（remove_for_entity 需要实体仍存活；destroy 钩子解绑
        // 事件订阅，防后续派发悬空）。
        foreach (var handle in _scriptHandles.Values)
        {
            try { _bridge.ScriptDestroyForEntity(handle); }
            catch { /* engine might be torn down already */ }
        }
        _scriptHandles.Clear();

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

        // 稳定键注册表随场景拆除清空（场景关闭；Sync 重同步前亦经此清空，
        // 随后 Sync 尾部以新快照重推）。
        _bridge.ClearEntityIdentities();
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

		if (_scriptHandles.Remove(entity.EntityGuid.Value, out var scriptHandle))
		{
			try { _bridge.ScriptDestroyForEntity(scriptHandle); }
			catch { /* ignore: engine might be torn down already */ }
		}
		if (_entities.GetEntityId(entity.EntityGuid) is { } runtimeId)
		{
			_bridge.DestroyEntity(runtimeId);
			_entities.Unregister(entity.EntityGuid);
		}
		_live.Remove(entity);

		// 映射已变（实体注销）：重推全量快照（幂等）。
		PushEntityIdentities();
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

            // 物理组件（M2-D）：collider 必须先于 rigidbody（EngineDLL 语义：
            // RigidBody 物化依赖同实体已存在的 Collider）
            foreach (var collider in entity.Components.OfType<ColliderComponent>())
            {
                _bridge.AddEntityCollider(runtimeId,
                    (uint)collider.Shape,
                    new[] { collider.HalfX, collider.HalfY, collider.HalfZ },
                    new[] { 0f, 0f, 0f },
                    collider.IsTrigger,
                    (uint)collider.Layer);
            }

            foreach (var rigidBody in entity.Components.OfType<RigidBodyComponent>())
            {
                _bridge.AddEntityRigidBody(runtimeId,
                    (uint)rigidBody.Mode,
                    rigidBody.Mass,
                    rigidBody.Restitution,
                    rigidBody.Friction,
                    new[] { rigidBody.VelocityX, rigidBody.VelocityY, rigidBody.VelocityZ });
            }

            // 粒子发射器（plan §6.3 B4 P1）：预设标量场填默认 config，
            // 覆盖 SpawnRate（blob offset 8 = emitter_config.spawn_rate），
            // 再为实体创建发射器。曲线字段由引擎侧 SanitizeConfig 置空。
            foreach (var emitter in entity.Components.OfType<ParticleEmitterComponent>())
            {
                if (!emitter.IsEnabled) continue;
                if (!_bridge.ParticleEnsureSystems()) break;
                var size = (int)_bridge.ParticleEmitterConfigSize();
                if (size <= 0) break;
                var config = new byte[size];
                // preset_manager::get_preset 是精确匹配，且引擎内置名首字母
                // 大写（"Fire"/"Dust"/…）。历史场景里可能存着小写 "fire"，
                // 先按不区分大小写对齐到引擎列表里的规范名再应用。
                var presetName = ResolvePresetName(emitter.PresetName);
                if (!_bridge.ParticlePresetApplyToConfig(presetName, config) &&
                    !_bridge.ParticlePresetApplyToConfig("Fire", config))
                {
                    Logger.Warning($"[Particle] preset '{presetName}' unavailable, entity '{entity.Name}' skipped");
                    break;
                }
                BitConverter.GetBytes(emitter.SpawnRate).CopyTo(config, 8);
                var emitterId = _bridge.EmitterCreateForEntity(runtimeId, config);
                if (emitterId == 0)
                    Logger.Warning($"[Particle] emitter creation failed for '{entity.Name}'");
                else
                    Logger.Info($"[Particle] emitter {emitterId} created for '{entity.Name}' (preset={presetName})");
            }

            // PFX 可编程粒子框架（N1 基础接线）：按资产路径创建发射器并应用暴露参数。
            foreach (var pfx in entity.Components.OfType<ParticleFXComponent>())
            {
                if (!pfx.IsEnabled) continue;
                if (string.IsNullOrWhiteSpace(pfx.AssetPath))
                {
                    Logger.Warning($"[ParticleFX] entity '{entity.Name}' has no AssetPath, skipped");
                    continue;
                }
                if (!_bridge.PfxSystemEnsure()) break;
                var pfxEmitterId = _bridge.PfxEmitterCreateFromAsset(pfx.AssetPath);
                if (pfxEmitterId == 0)
                {
                    Logger.Warning($"[ParticleFX] asset '{pfx.AssetPath}' failed to load for '{entity.Name}'");
                    continue;
                }
                RegisterPfxRuntimeEmitter(runtimeId, pfx.AssetPath, pfxEmitterId);
                var emitTransform = entity.GetTransform();
                if (emitTransform is not null)
                    _bridge.PfxSetEmitterPosition(pfxEmitterId,
                        emitTransform.PositionX, emitTransform.PositionY, emitTransform.PositionZ);
                foreach (var (paramName, paramValue) in pfx.ExposedParams)
                {
                    _bridge.PfxSetExposedParam(pfxEmitterId, PfxHash.HashName(paramName), paramValue);
                }
                Logger.Info($"[ParticleFX] emitter {pfxEmitterId} created for '{entity.Name}' (asset={pfx.AssetPath})");
            }

            // scripting S1：Lua 脚本组件（编辑态只挂实例不 tick——编辑器
            // EngineTick 不驱动 PipelineTick，运行语义在 Player/runtime 预览）。
            EnsureScript(entity, runtimeId);

            BindEditorHelperMesh(entity, runtimeId);

            var transform = entity.GetTransform();
            if (transform is null) continue;
            _bridge.SetEntityTransform(runtimeId,
                new[] { transform.PositionX, transform.PositionY, transform.PositionZ },
                new[] { transform.RotationX, transform.RotationY, transform.RotationZ },
                new[] { transform.ScaleX, transform.ScaleY, transform.ScaleZ });

            // PFX 发射器跟随实体位置（拖拽 gizmo / 属性面板改位置即生效）
            if (entity.GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>() != null
                && PfxEmitterByRuntime.TryGetValue(runtimeId, out var pfxId))
            {
                _bridge.PfxSetEmitterPosition(pfxId,
                    transform.PositionX, transform.PositionY, transform.PositionZ);
            }
        }

        // 稳定键注册表（2026-11-05 lightmap 稳定键计划）：场景激活/加载完成
        //（全部实体创建、网格绑定后）全量推送。
        PushEntityIdentities();
    }

    // ---- 实体稳定键注册表推送（2026-11-05 lightmap 稳定键计划） ----
    // lightmap bake 的 per-instance 键烧的是易失 runtime id（mesh_<h>_e<id>），
    // 产物 lightmap.lmap 跨会话失配。修复：编辑器把 runtime native entity id →
    // 实体 GUID 稳定键的全量快照批量推给引擎（引擎侧按 native id upsert，全量
    // 推送天然幂等）。推送时机 = 实体/网格生命周期节点：Sync（场景激活）、
    // SyncEntity（实体创建）、RemoveEntity（实体删除）、RefreshEntityMaterials/
    // RefreshEntityMesh（网格重绑会更换管线 mesh 实体 id）、Teardown（清空）。

    /// <summary>稳定键 = "e" + 实体 GUID 的 N 格式（32 小写 hex，总 33 字符）。</summary>
    internal static string BuildEntityStableKey(EntityGuid guid) => "e" + guid.Value.ToString("N");

    /// <summary>
    /// 构造稳定键注册表全量快照（纯函数，测试直测）：遍历
    /// <see cref="RuntimeEntityMap"/> 的全部 guid → runtime id 映射，经桥解析
    /// 出管线 mesh 实体 id（bake 键 e&lt;id&gt; 里的 id 即它，而非 source ECS
    /// id）。只有 mesh 实体才有管线侧实体——无 id 的实体跳过。
    /// </summary>
    internal static NativeEngineBridge.EntityIdentityC[] BuildIdentitySnapshot(
        RuntimeEntityMap entities, INativeEngineBridgeLike bridge)
    {
        var snapshot = new List<NativeEngineBridge.EntityIdentityC>();
        foreach (var (guid, nativeId) in entities.Enumerate())
        {
            if (nativeId == 0 || nativeId == NativeEngineBridge.InvalidEntityId) continue;
            // 只有 mesh 实体才有管线侧实体（PipelineRegisterMeshEntity 路径）；
            // 灯光/相机等纯组件实体不参与 bake，跳过。
            if (bridge.TryGetRenderMeshEntityId(nativeId) is not { } meshEntityId) continue;
            snapshot.Add(new NativeEngineBridge.EntityIdentityC
            {
                NativeEntityId = meshEntityId,
                StableKey = BuildEntityStableKey(guid),
            });
        }
        return snapshot.ToArray();
    }

    /// <summary>
    /// 推送全量快照（幂等）。失败仅记日志不抛异常——引擎注册表保持旧态时
    /// bake 回退易失 runtime id 键（现行为），不阻塞场景同步。
    /// </summary>
    private void PushEntityIdentities()
    {
        if (!_bridge.IsLoaded) return;
        try
        {
            var snapshot = BuildIdentitySnapshot(_entities, _bridge);
            if (!_bridge.SetEntityIdentities(snapshot))
                Logger.Warning(
                    $"SceneRuntimeSynchronizer: identity push rejected ({snapshot.Length} entries) — engine registry stale");
        }
        catch (Exception ex)
        {
            Logger.Error("SceneRuntimeSynchronizer: identity push failed", ex);
        }
    }

    /// <summary>
    /// scripting S1：把实体上的 LuaScriptComponent 挂成引擎脚本实例。
    /// 已有实例先销毁重建（ScriptSetParams 的宿主侧语义——begin_play 以新
    /// 参数重放）。GUID 解析失败/坏脚本：警告 + 跳过，不中断同步。
    /// </summary>
    private void EnsureScript(GameEntity entity, uint runtimeId)
    {
        var script = entity.GetComponent<Components.LuaScriptComponent>();
        if (script is null) return;

        // 无脚本资产 = 用户清空了引用：只拆不建。
        if (script.ScriptAssetGuid.Value == Guid.Empty)
        {
            if (_scriptHandles.Remove(entity.EntityGuid.Value, out var old))
                _bridge.ScriptDestroyForEntity(old);
            return;
        }
        var info = AssetRegistry.GetByGuid(script.ScriptAssetGuid.Value);
        if (info is null || string.IsNullOrEmpty(info.FullPath))
        {
            Logger.Warning($"[Script] asset not registered, entity '{entity.Name}' (guid={script.ScriptAssetGuid}) skipped");
            return;
        }
        if (_scriptHandles.Remove(entity.EntityGuid.Value, out var existing))
            _bridge.ScriptDestroyForEntity(existing);

        var keys = script.Parameters.Keys.ToArray();
        var values = script.Parameters.Values.ToArray();
        var handle = _bridge.ScriptCreateForEntity(runtimeId, info.FullPath, keys, values);
        if (handle == NativeEngineBridge.InvalidContentId)
        {
            Logger.Warning($"[Script] attach failed for '{entity.Name}' ({info.FullPath}) — see engine log");
            return;
        }
        _scriptHandles[entity.EntityGuid.Value] = handle;
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

    /// <summary>
    /// 按实体当前的 ParticleFX 组件状态重建其原生发射器（销毁旧的 → 依 AssetPath
    /// 重新编译创建 → 恢复位置/暴露参数）。资产换绑 / 保存热更新共用。
    /// </summary>
    public void ResyncPfxEmitter(GameEntity entity)
    {
        if (!_bridge.IsLoaded) return;
        var pfx = entity.GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>();
        if (pfx == null) return;
        if (_entities.GetEntityId(entity.EntityGuid) is not { } runtimeId) return;

        // 组件禁用/清空资产 → 只销毁旧发射器（无资产可重建）
        if (!pfx.IsEnabled || string.IsNullOrWhiteSpace(pfx.AssetPath))
        {
            if (PfxEmitterByRuntime.Remove(runtimeId, out var stale) && stale != 0)
                _bridge.PfxEmitterRemove(stale);
            PfxRuntimeEmitters.Remove(runtimeId);
            return;
        }

        if (PfxEmitterByRuntime.TryGetValue(runtimeId, out var oldId))
        {
            if (oldId != 0) _bridge.PfxEmitterRemove(oldId);
            PfxEmitterByRuntime.Remove(runtimeId);
            PfxRuntimeEmitters.Remove(runtimeId);
        }

        var newId = _bridge.PfxEmitterCreateFromAsset(pfx.AssetPath);
        if (newId == 0 || newId == uint.MaxValue)
        {
            Logger.Warning($"[ParticleFX] emitter (re)create failed for '{entity.Name}' (asset={pfx.AssetPath})");
            return;
        }
        RegisterPfxRuntimeEmitter(runtimeId, pfx.AssetPath, newId);

        var transform = entity.GetTransform();
        if (transform is not null)
            _bridge.PfxSetEmitterPosition(newId,
                transform.PositionX, transform.PositionY, transform.PositionZ);
        foreach (var (paramName, paramValue) in pfx.ExposedParams)
            _bridge.PfxSetExposedParam(newId, PfxHash.HashName(paramName), paramValue);

        Logger.Info($"[ParticleFX] emitter hot-reloaded for '{entity.Name}' " +
                    $"(asset={System.IO.Path.GetFileName(pfx.AssetPath)}, id={newId})");
    }

    /// <summary>
    /// .pfx_emitter 资产保存后的热更新：销毁所有使用该资产的活发射器，
    /// 按当前磁盘内容重新编译创建（位置/暴露参数沿用组件当前值）。
    /// 与 <see cref="ReloadMaterial"/> 同思路——PFX 编辑器 Save 后场景里的
    /// 发射器立即生效，无需重载场景或重新同步实体。
    /// </summary>
    public void ReloadPfxEmittersForAsset(string assetPath)
    {
        if (string.IsNullOrWhiteSpace(assetPath) || !_bridge.IsLoaded) return;
        string full;
        try { full = System.IO.Path.GetFullPath(assetPath); }
        catch (Exception ex)
        {
            Logger.Warning($"[ParticleFX] hot-reload: bad asset path '{assetPath}': {ex.Message}");
            return;
        }

        foreach (var entity in _live)
        {
            var pfx = entity.GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>();
            if (pfx == null || !pfx.IsEnabled || string.IsNullOrWhiteSpace(pfx.AssetPath)) continue;

            string entityAssetFull;
            try { entityAssetFull = System.IO.Path.GetFullPath(pfx.AssetPath); }
            catch { continue; }
            if (!string.Equals(entityAssetFull, full, StringComparison.OrdinalIgnoreCase)) continue;

            ResyncPfxEmitter(entity);
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

    private string? ResolveCookedArtifactPath(AssetInfo info) =>
        // 实现提取至 CookedArtifactResolver（PFX 发射器编辑器选 mesh 资产共用）
        CookedArtifactResolver.ResolveCookedPath(
            info, CookedPlatform, CookedConfiguration, UseSourceAsFallback);

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

        // 网格/材质重绑会更换管线 mesh 实体 id（SetEntityMeshTextures 内
        // unregister+register）——重推快照防 bake 键失配。
        PushEntityIdentities();
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
            // 网格解除后该实体退出稳定键快照：重推。
            PushEntityIdentities();
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
    /// <summary>
    /// 把组件里的预设名（可能为空或历史小写 "fire"）对齐到引擎预设管理器
    /// 里的规范名。内置名首字母大写（Engine/Particles/ParticlePreset.cpp
    /// default_presets），get_preset 是精确匹配。
    /// </summary>
    private string ResolvePresetName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) name = "Fire";
        return _bridge.ParticlePresetList()?.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                   .FirstOrDefault(p => string.Equals(p, name, StringComparison.OrdinalIgnoreCase))
               ?? name;
    }

    // ---- PFX 运行时发射器登记（编辑器热预览 / 实体位置同步用） ----------------
    // 按实体 runtime id 登记（同一资产可挂多个实体，各自独立发射器）；
    // 资产路径一并保存，供编辑器按资产查找"任意活发射器"做热预览。
    private static readonly System.Collections.Generic.Dictionary<uint, (string AssetPath, uint EmitterId)>
        PfxRuntimeEmitters = new();

    // runtimeId → emitterId 快速别名（PushTransforms 位置回填用）
    private static readonly System.Collections.Generic.Dictionary<uint, uint> PfxEmitterByRuntime = new();

    private void RegisterPfxRuntimeEmitter(uint runtimeId, string assetPath, uint emitterId)
    {
        if (runtimeId == 0 || emitterId == 0 || string.IsNullOrEmpty(assetPath)) return;
        PfxRuntimeEmitters[runtimeId] = (System.IO.Path.GetFullPath(assetPath), emitterId);
        PfxEmitterByRuntime[runtimeId] = emitterId;
    }

    /// <summary>某实体的 PFX 发射器 id（0 = 无）。</summary>
    public uint TryGetPfxEmitterIdForEntity(uint runtimeId) =>
        runtimeId != 0 && PfxRuntimeEmitters.TryGetValue(runtimeId, out var e) ? e.EmitterId : 0u;

    /// <summary>使用某 .pfx_emitter 资产的任意活发射器 id（0 = 无；多实体取首个）。</summary>
    public static uint TryGetPfxRuntimeEmitterId(string assetPath)
    {
        if (string.IsNullOrEmpty(assetPath)) return 0;
        try
        {
            var full = System.IO.Path.GetFullPath(assetPath);
            foreach (var (_, (path, emitterId)) in PfxRuntimeEmitters)
                if (string.Equals(path, full, StringComparison.OrdinalIgnoreCase) && emitterId != 0)
                    return emitterId;
            return 0u;
        }
        catch { return 0; }
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

            // 物理组件（M2-D）：collider 先于 rigidbody
            foreach (var collider in entity.Components.OfType<ColliderComponent>())
            {
                _bridge.AddEntityCollider(runtimeId,
                    (uint)collider.Shape,
                    new[] { collider.HalfX, collider.HalfY, collider.HalfZ },
                    new[] { 0f, 0f, 0f },
                    collider.IsTrigger,
                    (uint)collider.Layer);
            }

            foreach (var rigidBody in entity.Components.OfType<RigidBodyComponent>())
            {
                _bridge.AddEntityRigidBody(runtimeId,
                    (uint)rigidBody.Mode,
                    rigidBody.Mass,
                    rigidBody.Restitution,
                    rigidBody.Friction,
                    new[] { rigidBody.VelocityX, rigidBody.VelocityY, rigidBody.VelocityZ });
            }

            // scripting S1：增量路径同 Sync（拖放挂脚本/参数热改后即时生效）。
            EnsureScript(entity, runtimeId);

            BindEditorHelperMesh(entity, runtimeId);

            if (position != null)
            {
                _bridge.SetEntityTransform(runtimeId,
                    new[] { position.PositionX, position.PositionY, position.PositionZ },
                    new[] { position.RotationX, position.RotationY, position.RotationZ },
                    new[] { position.ScaleX, position.ScaleY, position.ScaleZ });
            }
        }

        // 新实体注册后全量重推（幂等；BindEditorHelperMesh 同样会产生管线
        // mesh，故在方法尾而非 RefreshEntityMaterials 一处收口）。
        PushEntityIdentities();
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
