using System;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

[Collection("AssetRegistry")]
public sealed class WorldEditorViewModelSynchronizationTests
{
    [Fact]
    public void SynchronizerRoundTrip_RegisterAllEntitiesAndBindings()
    {
        var entities = new RuntimeEntityMap();
        var assets = new RuntimeAssetMap();
        var materials = new RuntimeMaterialMap();
        var bridge = new RecordingBridge();
        var sync = new SceneRuntimeSynchronizer(bridge, entities, assets, materials);

        var scene = new Scene("Main");
        var entity = new GameEntity("Only");
        var meshGuid = new AssetGuid(Guid.NewGuid());
        var materialGuid = new AssetGuid(Guid.NewGuid());
        entity.Components.Add(new Transform { PositionX = 4, PositionY = 5, PositionZ = 6 });
        entity.Components.Add(new MeshRendererComponent
        {
            MeshGuid = meshGuid,
            MaterialGuids = { materialGuid }
        });
        scene.Entities.Add(entity);

        sync.Sync(scene);

        Assert.NotNull(entities.GetEntityId(entity.EntityGuid));
        Assert.Equal(1u, (uint)bridge.CreatedEntities);
        Assert.True(bridge.ImportMeshCalled);
        Assert.True(bridge.SetEntityMeshCalled);
        Assert.True(bridge.SetEntityTransformCalled);
        Assert.Equal(0xDEADBEEFUL, assets.GetContentId(meshGuid) ?? 0UL);
        Assert.NotNull(materials.GetRuntimeId(materialGuid));
    }

    private sealed class RecordingBridge : INativeEngineBridgeLike
    {
        public bool ParticleEnsureSystems() { return true; }
        public uint ParticleEmitterConfigSize() { return 304; }
        public uint EmitterCreateForEntity(uint entityId, byte[] config) { return 7; }
        public bool EmitterRemoveFromEntity(uint entityId) { return true; }
        public bool EmitterSetSpawnRate(uint entityId, float rate) { return true; }
        public string? ParticlePresetList() { return "fire"; }
        public bool ParticlePresetApplyToConfig(string name, byte[] config) { return true; }

        public bool IsLoaded => true;
        public uint CreatedEntities { get; private set; }
        public bool ImportMeshCalled { get; private set; }
        public bool SetEntityMeshCalled { get; private set; }
        public bool SetEntityTransformCalled { get; private set; }
        public uint NextEntityId { get; private set; }

        public uint CreateEntity(float x, float y, float z)
        {
            CreatedEntities++;
            return ++NextEntityId;
        }

        public void DestroyEntity(uint entityId) { }
        public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale) => SetEntityTransformCalled = true;
        public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) => 0;
        public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector) => 0;
        public ulong ImportMesh(string cookedPath) { ImportMeshCalled = true; return 0xDEADBEEFUL; }
        public uint CreateMaterial(AssetGuid shaderGuid) => ++NextEntityId;
        public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount) => SetEntityMeshCalled = true;
        public ulong CreateDefaultMaterial() => 0xABCDEFUL;
        public ulong ProceduralMeshCreate(uint meshType, float[] parameters) => 0xCAFEUL;
        public void ProceduralMeshDestroy(ulong contentId) { }
        public void ApplyMaterialGraph(uint entityId, uint graphId) { }
    }
}
