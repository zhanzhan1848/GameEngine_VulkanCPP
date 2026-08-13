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
        entity.Components.Add(new MeshRendererComponent()); // empty MeshGuid
        scene.Entities.Add(entity);

        sync.SyncEntity(entity);

        Assert.NotNull(entities.GetEntityId(entity.EntityGuid));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("proceduralCreate(")));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("createDefaultMaterial")));
        Assert.True(bridge.Calls.Any(c => c.StartsWith("setMesh(")));
    }

    [Fact]
    public void SyncEntity_WithNoMeshRenderer_DoesNotCreateBuiltinSphere()
    {
        var (sync, entities, _, _, bridge) = Build();
        var scene = new Scene("Main");
        sync.Sync(scene);

        var initialSphereCount = bridge.Calls.Count(c => c.StartsWith("proceduralCreate("));
        // Light-only entity: no MeshRenderer, so the synchronizer must
        // not allocate the placeholder sphere.
        var entity = new GameEntity("Light_1");
        entity.Components.Add(new Transform());
        entity.Components.Add(new LightComponent());
        scene.Entities.Add(entity);
        sync.SyncEntity(entity);

        Assert.Equal(initialSphereCount, bridge.Calls.Count(c => c.StartsWith("proceduralCreate(")));
    }

    [Fact]
    public void Teardown_ReleasesBuiltinSphereGeometry()
    {
        var (sync, _, _, _, bridge) = Build();
        var scene = new Scene("Main");
        // Trigger sphere creation via an entity with empty MeshGuid.
        var entity = new GameEntity("Char");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent());
        scene.Entities.Add(entity);
        sync.Sync(scene);
        sync.SyncEntity(entity);

        // Teardown must release the procedural sphere; otherwise it
        // leaks across scene activations.
        sync.Teardown();
        Assert.True(bridge.Calls.Any(c => c.StartsWith("proceduralDestroy(")));
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
        public bool IsLoaded { get; init; }
        public uint NextEntityId { get; set; } = 1;
        public uint NextMaterialId { get; set; } = 100;
        public System.Collections.Generic.HashSet<uint> Alive { get; } = new();
        public System.Collections.Generic.List<string> Calls { get; } = new();

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
        public ulong ImportMesh(string cookedPath) { Calls.Add($"importMesh({cookedPath})"); return 0xABCDUL; }
        public uint CreateMaterial(AssetGuid shaderGuid) { Calls.Add("material"); return NextMaterialId++; }
        public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount) { Calls.Add($"setMesh({entityId},{materialCount})"); }
        public ulong CreateDefaultMaterial() { Calls.Add("createDefaultMaterial"); return 0xFEEDFACEUL; }
        public ulong ProceduralMeshCreate(uint meshType, float[] parameters) { Calls.Add($"proceduralCreate({meshType})"); return 0xBEEFCAFEUL; }
        public void ProceduralMeshDestroy(ulong contentId) { Calls.Add($"proceduralDestroy({contentId})"); }
    }
}