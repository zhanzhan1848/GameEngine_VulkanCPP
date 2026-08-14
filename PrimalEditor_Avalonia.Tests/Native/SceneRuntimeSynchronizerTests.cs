using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

[Collection("AssetRegistry")]
public sealed class SceneRuntimeSynchronizerTests
{
    private sealed class FakeBridge : INativeEngineBridgeLike
    {
        public bool IsLoaded { get; init; }
        public uint NextEntityId { get; set; } = 1;
        public uint NextMaterialId { get; set; } = 100;
        public HashSet<uint> Alive { get; } = new();
        public List<string> Calls { get; } = new();

        public uint CreateEntity(float x, float y, float z)
        {
            Calls.Add($"create({x:0.##},{y:0.##},{z:0.##})");
            var id = NextEntityId++;
            Alive.Add(id);
            return id;
        }

        public void DestroyEntity(uint entityId)
        {
            Calls.Add($"destroy({entityId})");
            Alive.Remove(entityId);
        }

        public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale)
        {
            Calls.Add($"transform({entityId},{position[0]:0.##},{position[1]:0.##},{position[2]:0.##})");
        }

        public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled)
        {
            Calls.Add($"light({entityId},{lightType})");
            return entityId;
        }

        public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector)
        {
            Calls.Add($"camera({entityId})");
            return entityId;
        }

        public ulong ImportMesh(string cookedPath)
        {
            Calls.Add($"importMesh({cookedPath})");
            return 0xABCDUL;
        }

        public uint CreateMaterial(AssetGuid shaderGuid)
        {
            Calls.Add($"material({shaderGuid})");
            return NextMaterialId++;
        }

        public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount)
        {
            Calls.Add($"setMesh({entityId},{materialCount})");
        }

        public ulong CreateDefaultMaterial()
        {
            Calls.Add("createDefaultMaterial");
            return 0xFEEDFACEUL;
        }

        public ulong ProceduralMeshCreate(uint meshType, float[] parameters)
        {
            Calls.Add($"proceduralCreate({meshType})");
            return 0xBEEFCAFEUL;
        }

        public void ProceduralMeshDestroy(ulong contentId)
        {
            Calls.Add($"proceduralDestroy({contentId})");
        }
    }

    private static (SceneRuntimeSynchronizer synchronizer, RuntimeEntityMap entities, RuntimeAssetMap assets, RuntimeMaterialMap materials, FakeBridge bridge) Build(bool loaded = true)
    {
        var bridge = new FakeBridge { IsLoaded = loaded };
        var entities = new RuntimeEntityMap();
        var assets = new RuntimeAssetMap();
        var materials = new RuntimeMaterialMap();
        return (new SceneRuntimeSynchronizer(bridge, entities, assets, materials), entities, assets, materials, bridge);
    }

    [Fact]
    public void Sync_RegistersAllEntitiesAndClearsOnTeardown()
    {
        var (sync, entities, _, _, bridge) = Build();
        var scene = new Scene("Main");
        var a = new GameEntity("A");
        a.Components.Add(new Transform { PositionX = 1, PositionY = 2, PositionZ = 3 });
        var b = new GameEntity("B");
        b.Components.Add(new Transform());
        scene.Entities.Add(a);
        scene.Entities.Add(b);

        sync.Sync(scene);

        var aId = entities.GetEntityId(a.EntityGuid) ?? 0u;
        var bId = entities.GetEntityId(b.EntityGuid) ?? 0u;
        Assert.NotEqual(0u, aId);
        Assert.NotEqual(0u, bId);
        Assert.NotEqual(aId, bId);
        Assert.Contains(aId, bridge.Alive);
        Assert.Contains(bId, bridge.Alive);

        sync.Teardown();

        Assert.Null(entities.GetEntityId(a.EntityGuid));
        Assert.Empty(bridge.Alive);
    }

    [Fact]
    public void Sync_BindsMeshMaterialsAndTransforms()
    {
        var (sync, entities, assets, materials, bridge) = Build();
        var scene = new Scene("Main");
        var entity = new GameEntity("Robot");
        var meshGuid = new AssetGuid(Guid.NewGuid());
        var mat1 = new AssetGuid(Guid.NewGuid());
        var mat2 = new AssetGuid(Guid.NewGuid());
        entity.Components.Add(new MeshRendererComponent
        {
            MeshGuid = meshGuid,
            MaterialGuids = { mat1, mat2 }
        });
        entity.Components.Add(new Transform { PositionY = 5 });
        entity.Components.Add(new LightComponent { Intensity = 1 });
        scene.Entities.Add(entity);

        sync.Sync(scene);

        var entityId = entities.GetEntityId(entity.EntityGuid);
        Assert.Equal(0xABCDUL, (ulong)(assets.GetContentId(meshGuid) ?? 0UL));
        Assert.NotNull(materials.GetRuntimeId(mat1));
        Assert.NotNull(materials.GetRuntimeId(mat2));
        var callList = bridge.Calls as IList<string> ?? bridge.Calls.ToList();
        Assert.True(callList.Any(c => c.StartsWith("setMesh(")));
        Assert.True(callList.Any(c => c.StartsWith("light(")));
        Assert.True(callList.Any(c => c.StartsWith("transform(")));
        Assert.NotEqual(0u, (uint)(entityId ?? 0u));
    }

    [Fact]
    public void Sync_BailsOutWhenBridgeNotLoaded()
    {
        var (sync, entities, assets, materials, bridge) = Build(loaded: false);
        var scene = new Scene("Main");
        var entity = new GameEntity("Only");
        entity.Components.Add(new Transform());
        scene.Entities.Add(entity);

        sync.Sync(scene);

        Assert.Null(entities.GetEntityId(entity.EntityGuid));
        Assert.Empty(bridge.Calls);
    }

    [Fact]
    public void PushTransforms_UsesOwningBridgeWithMappedRuntimeEntity()
    {
        var (sync, entities, _, _, bridge) = Build();
        var scene = new Scene("Main");
        var entity = new GameEntity("Plane");
        var transform = new Transform();
        entity.Components.Add(transform);
        scene.Entities.Add(entity);
        sync.Sync(scene);
        var runtimeId = entities.GetEntityId(entity.EntityGuid);
        Assert.NotNull(runtimeId);
        bridge.Calls.Clear();

        transform.PositionX = 2f;
        transform.PositionY = 3f;
        transform.PositionZ = 4f;
        sync.PushTransforms(new[] { entity });

        Assert.Equal(new[] { $"transform({runtimeId},2,3,4)" }, bridge.Calls);
    }

    [Fact]
    public void Teardown_DestroysAllLiveEntities()
    {
        var (sync, _, _, _, bridge) = Build();
        var scene = new Scene("Main");
        var a = new GameEntity("A");
        a.Components.Add(new Transform());
        var b = new GameEntity("B");
        b.Components.Add(new Transform());
        scene.Entities.Add(a);
        scene.Entities.Add(b);
        sync.Sync(scene);
        var beforeCount = bridge.Alive.Count;
        Assert.Equal(2, beforeCount);

        sync.Teardown();

        Assert.Empty(bridge.Alive);
    }
}
