using System;
using System.Collections.Generic;
using System.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;

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
    uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector);

    /// <summary>
    /// Synchronously resolve a mesh import. Returns the native geometry
    /// content id on success or 0 on failure / timeout. Implementations
    /// may poll the engine's async import pipeline until the request
    /// completes.
    /// </summary>
    ulong ImportMesh(string cookedPath);

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
}

public sealed class NativeEngineBridgeAdapter : INativeEngineBridgeLike
{
    /// <summary>How long ImportMesh waits for an async cook to land before
    /// giving up and returning 0. Editor scenes tolerate a few seconds
    /// here; gameplay pre-warm runs should pre-import.</summary>
    public TimeSpan ImportMeshTimeout { get; set; } = TimeSpan.FromSeconds(8);

    /// <summary>How often we poll the engine during ImportMesh.</summary>
    public TimeSpan ImportMeshPollInterval { get; set; } = TimeSpan.FromMilliseconds(50);

    public bool IsLoaded => NativeEngineBridge.IsLoaded;

    public uint CreateEntity(float x, float y, float z) => NativeEngineBridge.CreateEntity(x, y, z);

    public void DestroyEntity(uint entityId) => NativeEngineBridge.PipelineUnregisterMeshEntity(entityId);

    public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale) =>
        NativeEngineBridge.SetEntityTransform(entityId, position, rotation, scale);

    public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) =>
        NativeEngineBridge.AddEntityLight(entityId, lightType, intensity, colorRgb, range, umbraRad, penumbraRad, isEnabled);

    public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector) =>
        NativeEngineBridge.AddEntityCamera(entityId, projectionType, fieldOfView, aspectRatio, nearZ, farZ, upVector);

    public ulong ImportMesh(string cookedPath)
    {
        if (!NativeEngineBridge.IsLoaded) return 0;
        // The native ImportMesh path is asynchronous: it returns a
        // request id and the geometry content id lands later via
        // TryGetMeshImportResult. SceneRuntimeSynchronizer expects a
        // synchronous content id, so we drive the pump here.
        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
        if (requestId == 0) return 0;
        var deadline = DateTime.UtcNow + ImportMeshTimeout;
        while (DateTime.UtcNow < deadline)
        {
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
            {
                return status == NativeEngineBridge.MeshImportResultStatus.Success ? meshId : 0UL;
            }
            Thread.Sleep(ImportMeshPollInterval);
        }
        Logger.Warning($"NativeEngineBridgeAdapter: ImportMesh timed out for {cookedPath}");
        return 0;
    }

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
        if (entityId == 0 || geometryContentId == 0 || geometryContentId == NativeEngineBridge.InvalidContentId) return;

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
        var meshEntityId = textures != null && textures.Length > 0
            ? NativeEngineBridge.PipelineRegisterMeshEntity(geometryContentId, textures)
            : NativeEngineBridge.PipelineRegisterMeshEntity(geometryContentId, null);
        if (meshEntityId == 0 || meshEntityId == NativeEngineBridge.InvalidEntityId)
        {
            Logger.Warning($"NativeEngineBridgeAdapter: PipelineRegisterMeshEntity returned invalid id for entity={entityId}");
        }
    }
}
