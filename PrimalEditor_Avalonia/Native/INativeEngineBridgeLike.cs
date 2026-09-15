using System;
using System.Collections.Generic;
using System.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// In-process bridge that lets the editor synchronise source scene data
/// with the engine without leaking NativeEngineBridge P/Invoke calls into
/// tests. The default implementation forwards to the static bridge; tests
/// can substitute their own implementation.
/// </summary>
public interface INativeEngineBridgeLike
{
    bool IsLoaded { get; }
    uint CreateEntity(float x, float y, float z);
    void DestroyEntity(uint entityId);
    void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale);
    uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled);
    bool ParticleEnsureSystems();
    uint ParticleEmitterConfigSize();
    uint EmitterCreateForEntity(uint entityId, byte[] config);
    bool EmitterRemoveFromEntity(uint entityId);
    bool EmitterSetSpawnRate(uint entityId, float rate);
    string? ParticlePresetList();
    bool ParticlePresetApplyToConfig(string name, byte[] config);

    /// <summary>
    /// Push the full light state for live Inspector edits (no scene resync).
    /// Default no-op preserves compatibility with test bridges.
    /// </summary>
    void SetEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) { }

    // ---- PFX（可编程粒子框架）；默认实现保持测试替身兼容 ----

    bool PfxSystemEnsure() => false;
    uint PfxEmitterCreateFromAsset(string path) => 0u;
    uint PfxSetEmitterPosition(uint emitterId, float x, float y, float z) => 0u;
    uint PfxEmitterRemove(uint emitterId) => 0u;
    uint PfxEmitterReloadFromMemory(uint emitterId, byte[] text) => 0u;
    uint PfxSetExposedParam(uint emitterId, uint nameHash, float value) => 0u;
    uint PfxGetEmitterStats(uint emitterId, out uint active, out uint capacity) { active = 0; capacity = 0; return 0u; }

    uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector);

    // === 物理组件（M2-D） ===
    uint AddEntityCollider(uint entityId, uint shape, float[] halfExtents, float[] offset, bool isTrigger, uint layer);
    uint RemoveEntityCollider(uint entityId);
    uint AddEntityRigidBody(uint entityId, uint mode, float mass, float restitution, float friction, float[] velocity);
    uint RemoveEntityRigidBody(uint entityId);
    bool EntityHasCollider(uint entityId);
    bool EntityHasRigidBody(uint entityId);

    /// <summary>
    /// Synchronously resolve a mesh import. Returns the native geometry
	/// content id on success or <see cref="NativeEngineBridge.InvalidContentId"/>
	/// on failure / timeout. Content id zero is valid. Implementations
    /// may poll the engine's async import pipeline until the request
    /// completes.
    /// </summary>
    ulong ImportMesh(string cookedPath);

    /// <summary>Load an engine-cooked .texture/TEXR artifact.</summary>
    ulong ImportCookedTexture(string cookedPath) => NativeEngineBridge.InvalidContentId;

    /// <summary>
    /// Allocate a runtime material handle for the given shader Guid.
    /// Returns 0 when the engine cannot satisfy the request (e.g. shader
    /// is not registered yet, or the asset system is uninitialised).
    /// </summary>
    uint CreateMaterial(AssetGuid shaderGuid);

    /// <summary>
    /// Attach a cooked mesh geometry plus its per-submesh materials to
    /// an already-created entity. The geometry content id comes from
    /// <see cref="ImportMesh"/>; each material id comes from
    /// <see cref="CreateMaterial"/>.
    /// </summary>
    void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount);

    /// <summary>
    /// Bind real texture content ids in Albedo/Normal/ORM order. The default
    /// implementation preserves compatibility with test bridges.
    /// </summary>
    void SetEntityMeshTextures(uint entityId, ulong geometryContentId, ulong[] textureContentIds)
    {
        var narrowed = new uint[textureContentIds.Length];
        for (var i = 0; i < textureContentIds.Length; i++) narrowed[i] = (uint)textureContentIds[i];
        SetEntityMesh(entityId, geometryContentId, narrowed, (uint)narrowed.Length);
    }

    /// <summary>Remove only the render-pipeline mesh bound to an editor entity.</summary>
    void UnbindEntityMesh(uint entityId) { }

    /// <summary>
    /// Allocate the engine's built-in default white PBR material. Used
    /// as a fallback when the user hasn't picked a shader — the editor
    /// has no Material system yet, so every unbound mesh renders with
    /// this. Returns 0 / invalid id when the engine cannot satisfy.
    /// </summary>
    ulong CreateDefaultMaterial();

    /// <summary>
    /// Apply the evaluated outputs of a material graph to the material
    /// already attached to <paramref name="entityId"/>. The graph is
    /// evaluated server-side; the resulting technique, scalars, base
    /// colour and texture bindings are pushed through the
    /// <c>SetMaterial*</c> setters exposed by the engine's
    /// <c>MaterialAPI</c>. No-op when the engine isn't loaded, when the
    /// graph id is invalid, or when evaluation fails.
    /// </summary>
    void ApplyMaterialGraph(uint entityId, uint graphId);

    /// <summary>
    /// Compile <paramref name="expandedGraphJson"/> (editor serialization with
    /// absolute texture paths) into a GBuffer fragment shader and attach it to
    /// the entity's world rendering. Returns false when compilation fails or
    /// the backend does not support graph materials; callers keep rendering
    /// through the fixed three-slot path in that case.
    /// Default implementation preserves compatibility with test bridges.
    /// </summary>
    bool SetEntityMaterialGraph(uint entityId, string expandedGraphJson) => false;

    /// <summary>Detach any previously registered compiled graph material.</summary>
    void ClearEntityMaterialGraph(uint entityId) { }

    /// <summary>
    /// Procedurally cook a primitive mesh (sphere, cube, capsule, …)
    /// and return its RHI content id. The id is valid until
    /// <see cref="ProceduralMeshDestroy"/> is called. Used by the
    /// editor to ship placeholder geometry for entities the user
    /// has created but hasn't assigned a real mesh yet.
    /// </summary>
    ulong ProceduralMeshCreate(uint meshType, float[] parameters);

    void ProceduralMeshDestroy(ulong contentId);
}

public sealed class NativeEngineBridgeAdapter : INativeEngineBridgeLike
{
    private readonly Dictionary<uint, ulong> _pipelineMeshEntities = new();
    /// <summary>How long ImportMesh waits for an async cook to land before
    /// giving up and returning 0. Editor scenes tolerate a few seconds
    /// here; gameplay pre-warm runs should pre-import.</summary>
    public TimeSpan ImportMeshTimeout { get; set; } = TimeSpan.FromSeconds(8);

    /// <summary>How often we poll the engine during ImportMesh.</summary>
    public TimeSpan ImportMeshPollInterval { get; set; } = TimeSpan.FromMilliseconds(50);

    public bool IsLoaded => NativeEngineBridge.IsLoaded;

    public uint CreateEntity(float x, float y, float z) => NativeEngineBridge.CreateEntity(x, y, z);

    public void DestroyEntity(uint entityId)
    {
        if (_pipelineMeshEntities.Remove(entityId, out var meshEntityId))
            NativeEngineBridge.PipelineUnregisterMeshEntity(meshEntityId);
        NativeEngineBridge.RemoveGameEntity(entityId);
    }

    public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale)
    {
        NativeEngineBridge.SetEntityTransform(entityId, position, rotation, scale);
        if (_pipelineMeshEntities.TryGetValue(entityId, out var meshEntityId))
            NativeEngineBridge.SetEntityTransform((uint)meshEntityId, position, rotation, scale);
    }

    public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) =>
        NativeEngineBridge.AddEntityLight(entityId, lightType, intensity, colorRgb, range, umbraRad, penumbraRad, isEnabled);
    public bool ParticleEnsureSystems() => NativeEngineBridge.ParticleEnsureSystems();
    public uint ParticleEmitterConfigSize() => NativeEngineBridge.ParticleEmitterConfigSize();
    public uint EmitterCreateForEntity(uint entityId, byte[] config) => NativeEngineBridge.EmitterCreateForEntity(entityId, config);
    public bool EmitterRemoveFromEntity(uint entityId) => NativeEngineBridge.EmitterRemoveFromEntity(entityId);
    public bool EmitterSetSpawnRate(uint entityId, float rate) => NativeEngineBridge.EmitterSetSpawnRate(entityId, rate);
    public string? ParticlePresetList() => NativeEngineBridge.ParticlePresetList();
    public bool ParticlePresetApplyToConfig(string name, byte[] config) => NativeEngineBridge.ParticlePresetApplyToConfig(name, config);

    public bool PfxSystemEnsure() => NativeEngineBridge.PfxSystemEnsure();
    public uint PfxEmitterCreateFromAsset(string path) => NativeEngineBridge.PfxEmitterCreateFromAsset(path);
    public uint PfxSetEmitterPosition(uint emitterId, float x, float y, float z) => NativeEngineBridge.PfxSetEmitterPosition(emitterId, x, y, z);
    public uint PfxEmitterRemove(uint emitterId) => NativeEngineBridge.PfxEmitterRemove(emitterId);
    public uint PfxEmitterReloadFromMemory(uint emitterId, byte[] text) => NativeEngineBridge.PfxEmitterReloadFromMemory(emitterId, text);
    public uint PfxSetExposedParam(uint emitterId, uint nameHash, float value) => NativeEngineBridge.PfxSetExposedParam(emitterId, nameHash, value);
    public uint PfxGetEmitterStats(uint emitterId, out uint active, out uint capacity) => NativeEngineBridge.PfxGetEmitterStats(emitterId, out active, out capacity);

    public void SetEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) =>
        NativeEngineBridge.SetEntityLight(entityId, lightType, intensity, colorRgb, range, umbraRad, penumbraRad, isEnabled);

    public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector) =>
        NativeEngineBridge.AddEntityCamera(entityId, projectionType, fieldOfView, aspectRatio, nearZ, farZ, upVector);

    // === 物理组件（M2-D） ===
    public uint AddEntityCollider(uint entityId, uint shape, float[] halfExtents, float[] offset, bool isTrigger, uint layer) =>
        NativeEngineBridge.AddEntityCollider(entityId, shape, halfExtents, offset, isTrigger, layer);
    public uint RemoveEntityCollider(uint entityId) =>
        NativeEngineBridge.RemoveEntityCollider(entityId);
    public uint AddEntityRigidBody(uint entityId, uint mode, float mass, float restitution, float friction, float[] velocity) =>
        NativeEngineBridge.AddEntityRigidBody(entityId, mode, mass, restitution, friction, velocity);
    public uint RemoveEntityRigidBody(uint entityId) =>
        NativeEngineBridge.RemoveEntityRigidBody(entityId);
    public bool EntityHasCollider(uint entityId) =>
        NativeEngineBridge.EntityHasCollider(entityId);
    public bool EntityHasRigidBody(uint entityId) =>
        NativeEngineBridge.EntityHasRigidBody(entityId);

    public ulong ImportMesh(string cookedPath)
    {
		if (!NativeEngineBridge.IsLoaded) return NativeEngineBridge.InvalidContentId;
        // The native ImportMesh path is asynchronous: it returns a
        // request id and the geometry content id lands later via
        // TryGetMeshImportResult. SceneRuntimeSynchronizer expects a
        // synchronous content id, so we drive the pump here.
        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
		if (requestId == 0) return NativeEngineBridge.InvalidContentId;
        var deadline = DateTime.UtcNow + ImportMeshTimeout;
        while (DateTime.UtcNow < deadline)
        {
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
            {
				return status == NativeEngineBridge.MeshImportResultStatus.Success
					? meshId
					: NativeEngineBridge.InvalidContentId;
            }
            Thread.Sleep(ImportMeshPollInterval);
        }
        Logger.Warning($"NativeEngineBridgeAdapter: ImportMesh timed out for {cookedPath}");
		return NativeEngineBridge.InvalidContentId;
    }

    public ulong ImportCookedTexture(string cookedPath) =>
        NativeEngineBridge.PipelineImportCookedTexture(cookedPath);

    public uint CreateMaterial(AssetGuid shaderGuid)
    {
        if (!NativeEngineBridge.IsLoaded) return 0;
        // v1 of the engine only exposes one default material; until
        // per-shader material allocation lands in the C++ API we route
        // every shader-bound material through CreateDefaultMaterial.
        // The shader Guid is logged for future inspection but not yet
        // consumed by the engine.
        if (shaderGuid.Value != Guid.Empty)
        {
            Logger.Info($"NativeEngineBridgeAdapter: binding default material to shader {shaderGuid}");
        }
        var materialId = NativeEngineBridge.CreateDefaultMaterial();
        if (materialId == 0 || materialId == NativeEngineBridge.InvalidContentId) return 0;
        return (uint)materialId;
    }

    public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (entityId == NativeEngineBridge.InvalidEntityId ||
            geometryContentId == NativeEngineBridge.InvalidContentId) return;

        // PipelineRegisterMeshEntity takes the texture/material runtime
        // ids as ulong[]. The interface exposes uint[] for symmetry
        // with material handles elsewhere; we widen here.
        ulong[]? textures = null;
        if (materialRuntimeIds != null && materialRuntimeIds.Length > 0)
        {
            textures = new ulong[materialRuntimeIds.Length];
            for (int i = 0; i < materialRuntimeIds.Length; i++)
                textures[i] = materialRuntimeIds[i];
        }

        // v1 artifact: PipelineRegisterMeshEntity returns a new mesh-entity
        // id rather than re-using the editor-side handle — the C++ side
        // matches geometry id, not entity tag. Register and log so the
        // caller can store the result for teardown.
        if (_pipelineMeshEntities.Remove(entityId, out var oldMeshEntityId))
            NativeEngineBridge.PipelineUnregisterMeshEntity(oldMeshEntityId);

        var meshEntityId = textures != null && textures.Length > 0
            ? NativeEngineBridge.PipelineRegisterMeshEntity(geometryContentId, textures)
            : NativeEngineBridge.PipelineRegisterMeshEntity(geometryContentId, null);
        if (meshEntityId == NativeEngineBridge.InvalidEntityId)
        {
            Logger.Warning($"NativeEngineBridgeAdapter: PipelineRegisterMeshEntity returned invalid id for entity={entityId}");
            return;
        }
        _pipelineMeshEntities[entityId] = meshEntityId;
    }

    public void SetEntityMeshTextures(uint entityId, ulong geometryContentId, ulong[] textureContentIds)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (entityId == NativeEngineBridge.InvalidEntityId ||
            geometryContentId == NativeEngineBridge.InvalidContentId) return;

        if (_pipelineMeshEntities.Remove(entityId, out var oldMeshEntityId))
            NativeEngineBridge.PipelineUnregisterMeshEntity(oldMeshEntityId);

        var meshEntityId = NativeEngineBridge.PipelineRegisterMeshEntity(
            geometryContentId,
            textureContentIds.Length == 0 ? null : textureContentIds);
        if (meshEntityId == NativeEngineBridge.InvalidEntityId) return;
        _pipelineMeshEntities[entityId] = meshEntityId;
    }

    public void UnbindEntityMesh(uint entityId)
    {
        if (_pipelineMeshEntities.Remove(entityId, out var meshEntityId))
            NativeEngineBridge.PipelineUnregisterMeshEntity(meshEntityId);
    }

    public ulong CreateDefaultMaterial()
    {
        if (!NativeEngineBridge.IsLoaded) return 0;
        return NativeEngineBridge.CreateDefaultMaterial();
    }

    public void ApplyMaterialGraph(uint entityId, uint graphId)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (entityId == 0 || graphId == 0) return;

        // Evaluate the graph to a populated material_init_info struct.
        // The session is adopted (not owned) so we don't double-free.
        using var session = MaterialGraphSession.Adopt(graphId);

        var materialEntityId = _pipelineMeshEntities.TryGetValue(entityId, out var pipelineEntityId)
            ? (uint)pipelineEntityId
            : entityId;

        // World-material path: compile the full graph (arbitrary topology,
        // up to 8 textures) into a GBuffer fragment shader and attach it to
        // this entity. The serialized JSON gets absolute texture paths via
        // the same expansion the preview pane uses; the disk sidecar stays
        // portable. On failure the scalar snapshot below still applies and
        // the entity keeps the fixed three-slot material.
        try
        {
            var json = session.SerializeJson();
            var expanded = PrimalEditor_Avalonia.Editors.Material.MaterialGraphSessionLocator
                .PreparePreviewJson(json ?? string.Empty);
            if (!string.IsNullOrEmpty(expanded) &&
                !NativeEngineBridge.PipelineSetEntityMaterialGraph(materialEntityId, expanded))
            {
                Logger.Warning($"ApplyMaterialGraph: entity {entityId} keeps fixed material (graph compile failed)");
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"ApplyMaterialGraph: world push skipped ({ex.Message})");
        }

        if (!session.TryEvaluate(out var info)) return;

        if (info.Technique != 0)
            NativeEngineBridge.SetMaterialTechnique(materialEntityId, info.Technique);
        NativeEngineBridge.SetMaterialRoughness(materialEntityId, info.Roughness);
        NativeEngineBridge.SetMaterialMetallic(materialEntityId, info.Metallic);
        NativeEngineBridge.SetMaterialAlphaCutoff(materialEntityId, info.AlphaCutoff);
        NativeEngineBridge.SetMaterialBaseColor(materialEntityId,
            info.BaseColorR, info.BaseColorG, info.BaseColorB, info.BaseColorA);
        if (info.AlbedoTexture != 0)
            NativeEngineBridge.SetMaterialAlbedoTexture(materialEntityId, info.AlbedoTexture);
        if (info.NormalTexture != 0)
            NativeEngineBridge.SetMaterialNormalTexture(materialEntityId, info.NormalTexture);
        if (info.OrmTexture != 0)
            NativeEngineBridge.SetMaterialOrmTexture(materialEntityId, info.OrmTexture);
    }

    public bool SetEntityMaterialGraph(uint entityId, string expandedGraphJson) =>
        NativeEngineBridge.PipelineSetEntityMaterialGraph(entityId, expandedGraphJson);

    public void ClearEntityMaterialGraph(uint entityId) =>
        NativeEngineBridge.PipelineClearEntityMaterialGraph(entityId);

    public ulong ProceduralMeshCreate(uint meshType, float[] parameters)
    {
        if (!NativeEngineBridge.IsLoaded) return 0;
        return NativeEngineBridge.ProceduralMeshCreate(meshType, parameters);
    }

    public void ProceduralMeshDestroy(ulong contentId)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (contentId == NativeEngineBridge.InvalidContentId) return;
        NativeEngineBridge.ProceduralMeshDestroy(contentId);
    }
}
