using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// 实体稳定键注册表推送（2026-11-05 lightmap 稳定键计划）单测：
/// 稳定键构造（"e"+guid N 格式）、全量快照构造过滤（无管线 mesh 的实体
/// 跳过、数组尺寸）、同步器生命周期落点（Sync/SyncEntity/RemoveEntity/
/// Teardown）的推送与清空调用。
/// </summary>
public sealed class EntityIdentitySnapshotTests
{
    /// <summary>
    /// 记录型测试桥：SetEntityMesh 模拟 NativeEngineBridgeAdapter 的
    /// PipelineRegisterMeshEntity 行为（source id → 独立 mesh 实体 id），
    /// 并记录稳定键快照推送 / 清空调用。
    /// </summary>
    private sealed class IdentityRecordingBridge : INativeEngineBridgeLike
    {
        public bool IsLoaded { get; init; } = true;
        private uint _next;
        public Dictionary<uint, uint> MeshEntities { get; } = new();
        public List<NativeEngineBridge.EntityIdentityC[]> Pushes { get; } = new();
        public int Clears { get; private set; }

        public uint CreateEntity(float x, float y, float z) => ++_next;
        public void DestroyEntity(uint entityId) => MeshEntities.Remove(entityId);
        public void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale) { }
        public void SetEntityMesh(uint entityId, ulong geometryContentId, uint[] materialRuntimeIds, uint materialCount)
            => MeshEntities[entityId] = 9000 + entityId;
        public void UnbindEntityMesh(uint entityId) => MeshEntities.Remove(entityId);
        public ulong CreateDefaultMaterial() => 1;
        public ulong ProceduralMeshCreate(uint meshType, float[] parameters) => 0x10;
        public void ProceduralMeshDestroy(ulong contentId) { }
        public ulong ImportMesh(string cookedPath) => 0x20;
        public uint CreateMaterial(AssetGuid shaderGuid) => 1;
        public void ApplyMaterialGraph(uint entityId, uint graphId) { }

        public uint? TryGetRenderMeshEntityId(uint entityId) =>
            MeshEntities.TryGetValue(entityId, out var meshId) ? meshId : null;
        public bool SetEntityIdentities(NativeEngineBridge.EntityIdentityC[] identities)
        {
            Pushes.Add(identities);
            return true;
        }
        public void ClearEntityIdentities() => Clears++;

        // 其余抽象成员：本测试面不触及——防误用即抛。
        public uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled) => throw new NotSupportedException();
        public bool ParticleEnsureSystems() => throw new NotSupportedException();
        public uint ParticleEmitterConfigSize() => throw new NotSupportedException();
        public uint EmitterCreateForEntity(uint entityId, byte[] config) => throw new NotSupportedException();
        public bool EmitterRemoveFromEntity(uint entityId) => throw new NotSupportedException();
        public bool EmitterSetSpawnRate(uint entityId, float rate) => throw new NotSupportedException();
        public string? ParticlePresetList() => throw new NotSupportedException();
        public bool ParticlePresetApplyToConfig(string name, byte[] config) => throw new NotSupportedException();
        public uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector) => throw new NotSupportedException();
        public uint AddEntityCollider(uint entityId, uint shape, float[] halfExtents, float[] offset, bool isTrigger, uint layer) => throw new NotSupportedException();
        public uint RemoveEntityCollider(uint entityId) => throw new NotSupportedException();
        public uint AddEntityRigidBody(uint entityId, uint mode, float mass, float restitution, float friction, float[] velocity) => throw new NotSupportedException();
        public uint RemoveEntityRigidBody(uint entityId) => throw new NotSupportedException();
        public bool EntityHasCollider(uint entityId) => throw new NotSupportedException();
        public bool EntityHasRigidBody(uint entityId) => throw new NotSupportedException();
    }

    private static (SceneRuntimeSynchronizer sync, RuntimeEntityMap entities, IdentityRecordingBridge bridge) Build(bool loaded = true)
    {
        var bridge = new IdentityRecordingBridge { IsLoaded = loaded };
        var entities = new RuntimeEntityMap();
        return (new SceneRuntimeSynchronizer(bridge, entities, new RuntimeAssetMap(), new RuntimeMaterialMap()),
            entities, bridge);
    }

    private static GameEntity MakeMeshEntity(string name)
    {
        var entity = new GameEntity(name);
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Box });
        return entity;
    }

    // ---- 稳定键构造 ----

    [Fact]
    public void StableKey_IsEPrefixPlusGuidNFormat()
    {
        var guid = new EntityGuid(Guid.NewGuid());
        var key = SceneRuntimeSynchronizer.BuildEntityStableKey(guid);
        Assert.Equal("e" + guid.Value.ToString("N"), key);
        // 33 字符（"e" + 32 小写 hex），含 NUL 共 34B ≤ char[64] 容量。
        Assert.Equal(33, key.Length);
        Assert.All(key.Skip(1), c => Assert.True(c is >= '0' and <= '9' or >= 'a' and <= 'f'));
    }

    [Fact]
    public void StableKey_FixedSampleLocksFormat()
    {
        // 冻结契约：stable_key = "e" + guid.ToString("N")。固定样例锁格式，
        // 防 ToString 格式漂移（D/B/P 的连字符/括号都会破坏与引擎侧的键匹配）。
        var guid = new EntityGuid(Guid.Parse("0123456789abcdef4242424242424242"));
        Assert.Equal("e0123456789abcdef4242424242424242",
            SceneRuntimeSynchronizer.BuildEntityStableKey(guid));
    }

    // ---- ABI 镜像 ----

    [Fact]
    public void EntityIdentityC_MatchesFrozenAbiLayout()
    {
        Assert.Equal(NativeEngineBridge.EntityIdentityC.ExpectedSizeBytes,
            Marshal.SizeOf<NativeEngineBridge.EntityIdentityC>());
        Assert.Equal(68, NativeEngineBridge.EntityIdentityC.ExpectedSizeBytes);
    }

    // ---- 快照构造（纯函数） ----

    [Fact]
    public void BuildIdentitySnapshot_FiltersNonMeshEntitiesAndKeepsSize()
    {
        var (_, entities, bridge) = Build();
        var meshA = new EntityGuid(Guid.NewGuid());
        var meshB = new EntityGuid(Guid.NewGuid());
        var bare = new EntityGuid(Guid.NewGuid());
        entities.Register(meshA, 10);
        entities.Register(meshB, 11);
        entities.Register(bare, 12);
        bridge.MeshEntities[10] = 1000;   // 仅 mesh 实体有管线侧 id
        // 边界：管线 mesh id 0 合法（EngineDLL RenderPipelineAPI.cpp 注释：
        // "0 is a VALID entity id (first slot allocated by ECS)"），不得被过滤。
        bridge.MeshEntities[11] = 0;

        var snapshot = SceneRuntimeSynchronizer.BuildIdentitySnapshot(entities, bridge);

        // bare（无管线 mesh）被跳过：数组尺寸 = 2，不是 3。
        Assert.Equal(2, snapshot.Length);
        Assert.Contains(snapshot, e => e.NativeEntityId == 1000u
            && e.StableKey == SceneRuntimeSynchronizer.BuildEntityStableKey(meshA));
        Assert.Contains(snapshot, e => e.NativeEntityId == 0u
            && e.StableKey == SceneRuntimeSynchronizer.BuildEntityStableKey(meshB));
        Assert.DoesNotContain(snapshot, e => e.StableKey == SceneRuntimeSynchronizer.BuildEntityStableKey(bare));
    }

    [Fact]
    public void BuildIdentitySnapshot_EmptyMapProducesEmptyArray()
    {
        var (_, entities, bridge) = Build();
        Assert.Empty(SceneRuntimeSynchronizer.BuildIdentitySnapshot(entities, bridge));
    }

    // ---- 同步器生命周期落点 ----

    [Fact]
    public void Sync_PushesFullSnapshotAfterSceneLoad()
    {
        var (sync, entities, bridge) = Build();
        var scene = new Scene("Main");
        var meshEntity = MakeMeshEntity("Box");
        var bare = new GameEntity("Bare");
        bare.Components.Add(new Transform());
        scene.Entities.Add(meshEntity);
        scene.Entities.Add(bare);

        sync.Sync(scene);

        // Sync 全程恰一次全量推送（尾部落点）；非 mesh 实体不进快照。
        var push = Assert.Single(bridge.Pushes);
        var entry = Assert.Single(push);
        Assert.Equal("e" + meshEntity.EntityGuid.Value.ToString("N"), entry.StableKey);
        var sourceId = entities.GetEntityId(meshEntity.EntityGuid)!.Value;
        Assert.Equal(9000 + sourceId, entry.NativeEntityId);
        // Sync 首部的 Teardown 会清空一次注册表，随后由尾部推送重建。
        Assert.Equal(1, bridge.Clears);
    }

    [Fact]
    public void SyncEntity_PushesAfterEntityRegistration()
    {
        var (sync, _, bridge) = Build();
        var meshEntity = MakeMeshEntity("Dropped");

        sync.SyncEntity(meshEntity);

        // RefreshEntityMaterials（网格绑定后）+ SyncEntity 尾部各一次——全量
        // 快照幂等，双推是既定行为（helper glyph 由尾部那次覆盖）。
        Assert.Equal(2, bridge.Pushes.Count);
        Assert.All(bridge.Pushes, p => Assert.Single(p));
        Assert.Equal("e" + meshEntity.EntityGuid.Value.ToString("N"),
            bridge.Pushes[^1][0].StableKey);
    }

    [Fact]
    public void RemoveEntity_RepublishesSnapshotWithoutEntity()
    {
        var (sync, _, bridge) = Build();
        var meshEntity = MakeMeshEntity("DeleteMe");
        sync.SyncEntity(meshEntity);
        var pushesBefore = bridge.Pushes.Count;

        sync.RemoveEntity(meshEntity);

        Assert.Equal(pushesBefore + 1, bridge.Pushes.Count);
        Assert.Empty(bridge.Pushes[^1]);
    }

    [Fact]
    public void Teardown_ClearsEngineRegistry()
    {
        var (sync, _, bridge) = Build();
        var scene = new Scene("Main");
        scene.Entities.Add(MakeMeshEntity("Box"));
        sync.Sync(scene);
        var clearsBefore = bridge.Clears;

        sync.Teardown();

        Assert.Equal(clearsBefore + 1, bridge.Clears);
    }

    [Fact]
    public void Sync_BridgeNotLoaded_DoesNotPush()
    {
        var (sync, _, bridge) = Build(loaded: false);
        var scene = new Scene("Main");
        scene.Entities.Add(MakeMeshEntity("Box"));

        sync.Sync(scene);

        Assert.Empty(bridge.Pushes);
    }
}
