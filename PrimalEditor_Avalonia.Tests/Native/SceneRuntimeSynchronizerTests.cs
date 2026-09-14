using System;
using System.Collections.Generic;
using System.IO;
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
		public ulong MeshContentId { get; set; } = 0xABCDUL;
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
        public bool ParticleEnsureSystems() { Calls.Add("particle.ensure"); return true; }
        public uint ParticleEmitterConfigSize() { return 304; }
        public uint EmitterCreateForEntity(uint entityId, byte[] config) { Calls.Add($"particle.create({entityId})"); return 7; }
        public bool EmitterRemoveFromEntity(uint entityId) { Calls.Add($"particle.remove({entityId})"); return true; }
        public bool EmitterSetSpawnRate(uint entityId, float rate) { Calls.Add($"particle.spawn({entityId},{rate})"); return true; }
        public string? ParticlePresetList() { return "fire"; }
        public bool ParticlePresetApplyToConfig(string name, byte[] config) { Calls.Add($"particle.preset({name})"); return true; }

        public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector)
        {
            Calls.Add($"camera({entityId})");
            return entityId;
        }

        // === 物理组件（M2-D，测试桩） ===
        public uint AddEntityCollider(uint entityId, uint shape, float[] halfExtents, float[] offset, bool isTrigger, uint layer)
        {
            Calls.Add($"collider({entityId})");
            return 1;
        }

        public uint RemoveEntityCollider(uint entityId)
        {
            Calls.Add($"collider-remove({entityId})");
            return 1;
        }

        public uint AddEntityRigidBody(uint entityId, uint mode, float mass, float restitution, float friction, float[] velocity)
        {
            Calls.Add($"rigidbody({entityId})");
            return 1;
        }

        public uint RemoveEntityRigidBody(uint entityId)
        {
            Calls.Add($"rigidbody-remove({entityId})");
            return 1;
        }

        public bool EntityHasCollider(uint entityId) => true;
        public bool EntityHasRigidBody(uint entityId) => true;

        public ulong ImportMesh(string cookedPath)
        {
            Calls.Add($"importMesh({cookedPath})");
			return MeshContentId;
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

        public void UnbindEntityMesh(uint entityId) => Calls.Add($"unbindMesh({entityId})");

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

        public void ApplyMaterialGraph(uint entityId, uint graphId)
        {
            Calls.Add($"applyMaterialGraph({entityId},{graphId})");
        }

        public HashSet<uint> GraphClears { get; } = new();
        public bool SetEntityMaterialGraph(uint entityId, string json)
        {
            Calls.Add($"setEntityMaterialGraph({entityId},{json.Length})");
            return true;
        }
        void INativeEngineBridgeLike.ClearEntityMaterialGraph(uint entityId) => GraphClears.Add(entityId);

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
    public void RefreshEntityMesh_AfterInspectorClear_UnbindsOnlyPipelineMesh()
    {
        var (sync, entities, _, _, bridge) = Build();
        var entity = new GameEntity("Plane");
        entity.Components.Add(new Transform());
        var renderer = new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Plane };
        entity.Components.Add(renderer);
        sync.SyncEntity(entity);
        bridge.Calls.Clear();

        renderer.BuiltinMesh = BuiltinMeshKind.None;
        renderer.MeshGuid = new AssetGuid(Guid.Empty);
        sync.RefreshEntityMesh(entity);

        var runtimeId = entities.GetEntityId(entity.EntityGuid);
        Assert.NotNull(runtimeId);
        Assert.Contains($"unbindMesh({runtimeId})", bridge.Calls);
        Assert.DoesNotContain(bridge.Calls, call => call.StartsWith("destroy("));
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
    public void Sync_BindsFirstValidGeometryContentSlotZero()
	{
		var (sync, _, assets, _, bridge) = Build();
		bridge.MeshContentId = 0;
		var scene = new Scene("Reloaded");
		var meshGuid = new AssetGuid(Guid.NewGuid());
		var entity = new GameEntity("PersistedModel");
		entity.Components.Add(new Transform());
		entity.Components.Add(new MeshRendererComponent { MeshGuid = meshGuid });
		scene.Entities.Add(entity);

		sync.Sync(scene);

		Assert.Equal(0UL, assets.GetContentId(meshGuid));
        Assert.Contains(bridge.Calls, call => call.StartsWith("setMesh("));
    }

	[Fact]
	public void Sync_ReloadedModelSidecar_ImportsSiblingMeshAndBindsContentSlotZero()
	{
		var root = Path.Combine(Path.GetTempPath(), "PrimalReload_" + Guid.NewGuid().ToString("N"));
		var content = Path.Combine(root, "Content", "Assets");
		Directory.CreateDirectory(content);
		try
		{
			var sidecar = Path.Combine(content, "corridor-end.model.json");
			var mesh = Path.Combine(content, "corridor-end.mesh");
			File.WriteAllText(sidecar, "{}");
			File.WriteAllBytes(mesh, Array.Empty<byte>());
			AssetRegistry.Initialize(Path.Combine(root, "Content"), root);
			var info = AssetRegistry.Get(sidecar);
			Assert.NotNull(info);

			var (sync, _, assets, _, bridge) = Build();
			bridge.MeshContentId = 0;
			var scene = new Scene("Reloaded");
			var entity = new GameEntity("corridor-end");
			var meshGuid = new AssetGuid(info!.Guid);
			entity.Components.Add(new Transform());
			entity.Components.Add(new MeshRendererComponent { MeshGuid = meshGuid });
			scene.Entities.Add(entity);

			sync.Sync(scene);

			Assert.Contains($"importMesh({mesh})", bridge.Calls);
			Assert.Equal(0UL, assets.GetContentId(meshGuid));
			Assert.Contains(bridge.Calls, call => call.StartsWith("setMesh("));
		}
		finally
		{
			AssetRegistry.Reset();
			Directory.Delete(root, recursive: true);
		}
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

	[Fact]
	public void RemoveEntity_DestroysAndUnregistersRuntimeEntity()
	{
		var (sync, entities, _, _, bridge) = Build();
		var scene = new Scene("Main");
		var entity = new GameEntity("DeleteMe");
		entity.Components.Add(new Transform());
		scene.Entities.Add(entity);
		sync.Sync(scene);
		var runtimeId = entities.GetEntityId(entity.EntityGuid);
		Assert.NotNull(runtimeId);

		sync.RemoveEntity(entity);

		Assert.Null(entities.GetEntityId(entity.EntityGuid));
		Assert.DoesNotContain(runtimeId!.Value, bridge.Alive);
		Assert.Contains($"destroy({runtimeId.Value})", bridge.Calls);
	}

	[Fact]
	public void ReloadMaterial_EvictsCachedGraphAndReappliesToReferencingEntities()
	{
		var root = Path.Combine(Path.GetTempPath(), "PrimalMatReload_" + Guid.NewGuid().ToString("N"));
		var content = Path.Combine(root, "Content", "Assets");
		Directory.CreateDirectory(content);
		try
		{
			var materialPath = Path.Combine(content, "gold.material.json");
			File.WriteAllText(materialPath, "{}");
			File.WriteAllText(Path.ChangeExtension(materialPath, ".material.graph.json"), "{}");
			AssetRegistry.Initialize(content, root);
			var info = AssetRegistry.Get(materialPath);
			Assert.NotNull(info);

			var (sync, entities, _, _, bridge) = Build();
			var loads = 0;
			sync.MaterialGraphLoaderHook = _ => (uint)(500 + ++loads);

			var scene = new Scene("Main");
			var materialGuid = new AssetGuid(info!.Guid);
			var meshGuid = new AssetGuid(Guid.NewGuid());
			var entity = new GameEntity("Gold");
			entity.Components.Add(new Transform());
			entity.Components.Add(new MeshRendererComponent
			{
				MeshGuid = meshGuid,
				MaterialGuids = { materialGuid }
			});
			scene.Entities.Add(entity);
			// No material: a save of gold.material.json must not touch it.
			var bare = new GameEntity("Bare");
			bare.Components.Add(new Transform());
			entity.Children.Add(bare);
			scene.Entities.Add(bare);

			sync.Sync(scene);

			var runtimeId = entities.GetEntityId(entity.EntityGuid)!.Value;
			// Initial sync loads the sidecar once and binds its handle.
			Assert.Equal(1, loads);
			Assert.Contains($"applyMaterialGraph({runtimeId},501)", bridge.Calls);
			Assert.DoesNotContain(bridge.Calls, c => c.StartsWith("applyMaterialGraph(") && !c.EndsWith(",501)"));

			// Editor save → sidecar rewritten: the cached pre-save handle must
			// be evicted so the next bind reads the fresh file.
			File.WriteAllText(Path.ChangeExtension(materialPath, ".material.graph.json"), "{ \"changed\": true }");
			bridge.Calls.Clear();
			sync.ReloadMaterial(materialGuid);

			Assert.Equal(2, loads);
			Assert.Contains($"applyMaterialGraph({runtimeId},502)", bridge.Calls);
			var bareRuntimeId = entities.GetEntityId(bare.EntityGuid)!.Value;
			Assert.DoesNotContain(bridge.Calls, c => c.StartsWith($"applyMaterialGraph({bareRuntimeId},"));
		}
		finally
		{
			AssetRegistry.Reset();
			try { Directory.Delete(root, recursive: true); } catch { /* best effort */ }
		}
	}

	[Fact]
	public void RefreshEntityMaterials_ClearsStaleGraphBinding_WhenNoSlotResolvesAGraph()
	{
		// Material registered but no sidecar on disk: after the refresh no
		// slot yields a graph, so a previously attached compiled variant must
		// be dropped (the world render falls back to the fixed path).
		var root = Path.Combine(Path.GetTempPath(), "PrimalMatClear_" + Guid.NewGuid().ToString("N"));
		var content = Path.Combine(root, "Content", "Assets");
		Directory.CreateDirectory(content);
		try
		{
			var materialPath = Path.Combine(content, "plain.material.json");
			File.WriteAllText(materialPath, "{}");   // intentionally NO .material.graph.json sibling
			AssetRegistry.Initialize(content, root);
			var info = AssetRegistry.Get(materialPath);
			Assert.NotNull(info);

			var (sync, entities, _, _, bridge) = Build();

			var scene = new Scene("Main");
			var materialGuid = new AssetGuid(info!.Guid);
			var meshGuid = new AssetGuid(Guid.NewGuid());
			var entity = new GameEntity("Plain");
			entity.Components.Add(new Transform());
			entity.Components.Add(new MeshRendererComponent
			{
				MeshGuid = meshGuid,
				MaterialGuids = { materialGuid }
			});
			scene.Entities.Add(entity);

			sync.Sync(scene);

			var runtimeId = entities.GetEntityId(entity.EntityGuid)!.Value;
			Assert.Empty(bridge.GraphClears);   // initial sync never had a graph to clear

			sync.RefreshEntityMaterials(entity);

			Assert.Contains(runtimeId, bridge.GraphClears);
		}
		finally
		{
			AssetRegistry.Reset();
			try { Directory.Delete(root, recursive: true); } catch { /* best effort */ }
		}
	}
}
