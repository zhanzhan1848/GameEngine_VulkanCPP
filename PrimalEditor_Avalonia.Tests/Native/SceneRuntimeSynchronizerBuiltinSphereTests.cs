using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

[Collection("AssetRegistry")]
public sealed class SceneRuntimeSynchronizerBuiltinSphereTests
{
    [Fact]
    public void SyncEntity_WithEmptyMeshGuid_BindsBuiltinSphereAndDefaultMaterial()
    {
        var (sync, entities, assets, materials, bridge) = Build();
        var scene = new Scene("Main");
        sync.Sync(scene);

        // After the scene sync we add a Character-equivalent entity:
        // MeshRenderer with an empty MeshGuid. The synchronizer must
        // turn this into a visible sphere bound with the default white
        // PBR material, otherwise the editor's Render Surface stays
        // empty after the toolbar buttons run.
        var entity = new GameEntity("Character_1");
        entity.Components.Add(new Transform { PositionX = 1, PositionY = 2, PositionZ = 3 });
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Sphere });
        scene.Entities.Add(entity);

        sync.SyncEntity(entity);

        Assert.NotNull(entities.GetEntityId(entity.EntityGuid));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("proceduralCreate(")));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("createDefaultMaterial")));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("setMesh(")));
    }

    [Fact]
    public void SyncEntity_LightWithoutMeshRenderer_BindsEditorHelperMesh()
    {
        var (sync, entities, _, _, bridge) = Build();
        var scene = new Scene("Main");
        sync.Sync(scene);

        var entity = new GameEntity("Light_1");
        entity.Components.Add(new Transform());
        entity.Components.Add(new LightComponent());
        scene.Entities.Add(entity);
        sync.SyncEntity(entity);

        Assert.Contains("proceduralCreate(0)", bridge.Calls);
        Assert.Contains(bridge.Calls, c => c.StartsWith("setMesh("));
        Assert.Contains(bridge.Calls, c => c.StartsWith("transform("));
    }

    [Fact]
    public void SyncEntity_CameraWithoutMeshRenderer_BindsEditorHelperMesh()
    {
        var (sync, _, _, _, bridge) = Build();
        var entity = new GameEntity("Camera_1");
        entity.Components.Add(new Transform());
        entity.Components.Add(new CameraComponent());

        sync.SyncEntity(entity);

        Assert.Contains("proceduralCreate(3)", bridge.Calls);
        Assert.Contains(bridge.Calls, c => c.StartsWith("setMesh("));
        Assert.Contains(bridge.Calls, c => c.StartsWith("transform("));
    }

    [Fact]
    public void Teardown_ReleasesBuiltinSphereGeometry()
    {
        var (sync, _, _, _, bridge) = Build();
        var scene = new Scene("Main");
        // Trigger sphere creation via an entity with empty MeshGuid.
        var entity = new GameEntity("Char");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Sphere });
        scene.Entities.Add(entity);
        sync.Sync(scene);
        sync.SyncEntity(entity);

        // Teardown must release the procedural sphere; otherwise it
        // leaks across scene activations.
        sync.Teardown();
        Assert.True(bridge.Calls.Any(c => c.StartsWith("proceduralDestroy(")));
    }

    [Theory]
    [InlineData(BuiltinMeshKind.Sphere, 0u)]
    [InlineData(BuiltinMeshKind.Box, 3u)]
    [InlineData(BuiltinMeshKind.Plane, 9u)]
    [InlineData(BuiltinMeshKind.Capsule, 5u)]
    public void SyncEntity_BuiltinMesh_UsesMatchingProceduralType(BuiltinMeshKind kind, uint expectedType)
    {
        var (sync, _, _, _, bridge) = Build();
        var entity = new GameEntity(kind.ToString());
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = kind });

        sync.SyncEntity(entity);

        Assert.Contains($"proceduralCreate({expectedType})", bridge.Calls);
        Assert.Contains(bridge.Calls, c => c.StartsWith("setMesh("));
    }

    [Fact]
    public void SyncEntity_EmptyRendererWithoutBuiltin_DoesNotCreateProceduralMesh()
    {
        var (sync, _, _, _, bridge) = Build();
        var entity = new GameEntity("Empty renderer");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent());

        sync.SyncEntity(entity);

        Assert.DoesNotContain(bridge.Calls, c => c.StartsWith("proceduralCreate("));
        Assert.DoesNotContain(bridge.Calls, c => c.StartsWith("setMesh("));
    }

    [Fact]
    public void SyncEntity_BuiltinMesh_AcceptsNativeSlotZero()
    {
        var (sync, _, _, _, bridge) = Build();
        bridge.ProceduralResult = 0;
        var entity = new GameEntity("First native mesh");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Sphere });

        sync.SyncEntity(entity);

        Assert.Contains(bridge.Calls, c => c.StartsWith("setMesh("));
    }

    [Fact]
    public void RegisterPreImportedMesh_ReusesContentIdWithoutReImporting()
    {
        var (sync, _, assets, _, bridge) = Build();
        var guid = new AssetGuid(System.Guid.NewGuid());
        const ulong preImportedId = 0xDEADBEEFUL;
        sync.RegisterPreImportedMesh(guid, preImportedId);

        // Verify pre-registration took effect before exercising Sync.
        Assert.True(assets.GetContentId(guid).HasValue,
            $"Pre-registration should populate the asset map; got null for guid={guid}");
        Assert.Equal(preImportedId, assets.GetContentId(guid));

        // Drive a sync that uses the mesh via a real entity. The
        // synchronizer must NOT call ImportMesh for the pre-registered
        // guid.
        var scene = new Scene("Main");
        var entity = new GameEntity("Dropped");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { MeshGuid = guid });
        scene.Entities.Add(entity);

        // The DropAsset flow drives SyncEntity (single-entity push), not
        // the full Sync(scene) which Teardown()s the runtime maps. We
        // exercise the path the editor actually uses here.
        sync.SyncEntity(entity);

        var importCalls = bridge.Calls.Where(c => c.StartsWith("importMesh(")).ToList();
        Assert.False(importCalls.Any(),
            $"Expected no importMesh calls after pre-registration, got: {string.Join(", ", importCalls)}");
        Assert.True(bridge.Calls.Any(c => c.StartsWith("setMesh(")));
    }

	[Fact]
	public void RegisterPreImportedMesh_AcceptsFirstValidContentSlotZero()
	{
		var (sync, _, assets, _, _) = Build();
		var guid = new AssetGuid(System.Guid.NewGuid());

		sync.RegisterPreImportedMesh(guid, 0);

		Assert.Equal(0UL, assets.GetContentId(guid));
	}

    private static (SceneRuntimeSynchronizer sync, RuntimeEntityMap entities, RuntimeAssetMap assets, RuntimeMaterialMap materials, FakeBridge bridge) Build()
    {
        var bridge = new FakeBridge { IsLoaded = true };
        var entities = new RuntimeEntityMap();
        var assets = new RuntimeAssetMap();
        var materials = new RuntimeMaterialMap();
        return (new SceneRuntimeSynchronizer(bridge, entities, assets, materials), entities, assets, materials, bridge);
    }

    private sealed class FakeBridge : PrimalEditor_Avalonia.Native.INativeEngineBridgeLike
    {
        public bool ParticleEnsureSystems() { return true; }
        public uint ParticleEmitterConfigSize() { return 304; }
        public uint EmitterCreateForEntity(uint entityId, byte[] config) { return 7; }
        public bool EmitterRemoveFromEntity(uint entityId) { return true; }
        public bool EmitterSetSpawnRate(uint entityId, float rate) { return true; }
        public string? ParticlePresetList() { return "fire"; }
        public bool ParticlePresetApplyToConfig(string name, byte[] config) { return true; }

        public bool IsLoaded { get; init; }
        public uint NextEntityId { get; set; } = 1;
        public uint NextMaterialId { get; set; } = 100;
        public System.Collections.Generic.HashSet<uint> Alive { get; } = new();
        public System.Collections.Generic.List<string> Calls { get; } = new();
        public ulong ProceduralResult { get; set; } = 0xBEEFCAFEUL;

        public uint CreateEntity(float x, float y, float z)
        {
            Calls.Add($"create({x:0.##},{y:0.##},{z:0.##})");
            var id = NextEntityId++;
            Alive.Add(id);
            return id;
        }

        public void DestroyEntity(uint entityId) { Calls.Add($"destroy({entityId})"); Alive.Remove(entityId); }
        public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale) { Calls.Add($"transform({entityId})"); }
        public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) { Calls.Add($"light({entityId})"); return entityId; }
        public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector) { Calls.Add($"camera({entityId})"); return entityId; }

        // === 物理组件（M2-D，测试桩） ===
        public uint AddEntityCollider(uint entityId, uint shape, float[] halfExtents, float[] offset, bool isTrigger, uint layer) { Calls.Add($"collider({entityId})"); return 1; }
        public uint RemoveEntityCollider(uint entityId) { Calls.Add($"collider-remove({entityId})"); return 1; }
        public uint AddEntityRigidBody(uint entityId, uint mode, float mass, float restitution, float friction, float[] velocity) { Calls.Add($"rigidbody({entityId})"); return 1; }
        public uint RemoveEntityRigidBody(uint entityId) { Calls.Add($"rigidbody-remove({entityId})"); return 1; }
        public bool EntityHasCollider(uint entityId) => true;
        public bool EntityHasRigidBody(uint entityId) => true;
        public ulong ImportMesh(string cookedPath) { Calls.Add($"importMesh({cookedPath})"); return 0xABCDUL; }
        public uint CreateMaterial(AssetGuid shaderGuid) { Calls.Add("material"); return NextMaterialId++; }
        public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount) { Calls.Add($"setMesh({entityId},{materialCount})"); }
        public ulong CreateDefaultMaterial() { Calls.Add("createDefaultMaterial"); return 0xFEEDFACEUL; }
        public ulong ProceduralMeshCreate(uint meshType, float[] parameters) { Calls.Add($"proceduralCreate({meshType})"); return ProceduralResult; }
        public void ProceduralMeshDestroy(ulong contentId) { Calls.Add($"proceduralDestroy({contentId})"); }
        public void ApplyMaterialGraph(uint entityId, uint graphId) { Calls.Add($"applyMaterialGraph({entityId},{graphId})"); }
    }
}
