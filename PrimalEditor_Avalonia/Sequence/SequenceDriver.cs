using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>渲染/播放前对受轨驱动实体 pose 的快照（结束后恢复原状）。</summary>
public sealed class SequencePoseSnapshot
{
    public List<(GameEntity Entity, TransformData Data)> Entries { get; } = new();

    /// <summary>属性轨覆盖的原值（entity, component.property, value)——sequence S1。</summary>
    public List<(GameEntity Entity, string Component, string Property, float Value)> PropertyEntries { get; } = new();
}

/// <summary>
/// 求值→应用驱动器：把某一帧的求值结果经文档模型路径落到引擎
/// （TransformData.ApplyTo + PushTransformsToNative，与属性面板/entity.set-transform
/// 同一同步通道）。播放/seek/渲染是"播放态"，有意不推 undo——关键帧编辑才入栈。
/// </summary>
public static class SequenceDriver
{
    /// <summary>
    /// 求值 frame 并应用变换。返回（应用的实体数，cut 相机实体的 native id；
    /// 0 = 无 cut 命中或相机实体未注册）。
    /// </summary>
    public static (int Applied, uint CameraNativeId) ApplyFrame(
        SequenceDocument doc, int frame, WorldEditorViewModel worldEditor)
    {
        var snapshot = SequenceEvaluator.Evaluate(doc, frame);
        var scene = worldEditor.ActiveScene;
        var touched = new List<GameEntity>();
        foreach (var (guid, data) in snapshot.Transforms)
        {
            var entity = FindEntity(scene?.Entities, guid);
            var transform = entity?.GetTransform();
            if (entity == null || transform == null) continue; // 轨指向的实体已删除：跳过
            data.ApplyTo(transform);
            worldEditor.Properties.RefreshFromTransform(transform);
            touched.Add(entity);
        }
        if (touched.Count > 0)
            worldEditor.PushTransformsToNative(touched);

        // 属性轨（sequence S1）：light.intensity → 文档模型 + 原生 setter 同步
        foreach (var sample in snapshot.Properties)
        {
            var entity = FindEntity(scene?.Entities, sample.EntityGuid);
            if (entity == null) continue;   // 目标已删除：跳过（合同 A02）
            if (sample.Component == "light" && sample.Property == "intensity")
            {
                var light = entity.GetComponent<LightComponent>();
                if (light == null) continue;
                light.Intensity = sample.Value;
                var nativeId = worldEditor.GetNativeEntityId(entity);
                if (nativeId != 0 && PrimalEditor_Avalonia.Native.NativeEngineBridge.IsLoaded)
                    PrimalEditor_Avalonia.Native.NativeEngineBridge.SetEntityLightIntensity(nativeId, sample.Value);
            }
        }

        uint cameraNativeId = 0;
        if (snapshot.CameraEntityGuid.HasValue)
        {
            var cam = FindEntity(scene?.Entities, snapshot.CameraEntityGuid.Value);
            if (cam != null) cameraNativeId = worldEditor.GetNativeEntityId(cam);
        }
        return (touched.Count, cameraNativeId);
    }

    /// <summary>序列所有 Transform 轨目标实体的当前 pose 快照（渲染前调用）。</summary>
    public static SequencePoseSnapshot CapturePoses(SequenceDocument doc, WorldEditorViewModel worldEditor)
    {
        var snap = new SequencePoseSnapshot();
        var scene = worldEditor.ActiveScene;
        foreach (var track in doc.Tracks)
        {
            if (track is not TransformTrack tt) continue;
            var entity = FindEntity(scene?.Entities, tt.EntityGuid);
            var transform = entity?.GetTransform();
            if (entity == null || transform == null) continue;
            snap.Entries.Add((entity, TransformData.FromTransform(transform)));
        }
        foreach (var pt in doc.Tracks.OfType<PropertyTrack>())
        {
            if (pt.Keys.Count == 0 || !PropertyTrack.IsSupported(pt.Component, pt.Property)) continue;
            var entity = FindEntity(scene?.Entities, pt.EntityGuid);
            if (entity == null) continue;
            if (pt.Component == "light" && pt.Property == "intensity")
            {
                var light = entity.GetComponent<LightComponent>();
                if (light != null)
                    snap.PropertyEntries.Add((entity, pt.Component, pt.Property, light.Intensity));
            }
        }
        return snap;
    }

    /// <summary>恢复快照 pose 并推送引擎（录制结束时场景不留录制残留）。</summary>
    public static void RestorePoses(SequencePoseSnapshot snapshot, WorldEditorViewModel worldEditor)
    {
        if (snapshot.Entries.Count == 0) return;
        foreach (var (entity, data) in snapshot.Entries)
            if (entity.GetTransform() is { } t)
            {
                data.ApplyTo(t);
                worldEditor.Properties.RefreshFromTransform(t);
            }
        worldEditor.PushTransformsToNative(SnapshotEntities(snapshot));
        foreach (var (entity, component, property, value) in snapshot.PropertyEntries)
        {
            if (component == "light" && property == "intensity")
            {
                var light = entity.GetComponent<LightComponent>();
                if (light == null) continue;
                light.Intensity = value;
                var nativeId = worldEditor.GetNativeEntityId(entity);
                if (nativeId != 0 && PrimalEditor_Avalonia.Native.NativeEngineBridge.IsLoaded)
                    PrimalEditor_Avalonia.Native.NativeEngineBridge.SetEntityLightIntensity(nativeId, value);
            }
        }
    }

    private static IEnumerable<GameEntity> SnapshotEntities(SequencePoseSnapshot snapshot)
    {
        foreach (var (entity, _) in snapshot.Entries) yield return entity;
    }

    /// <summary>按 guid 深度查找实体（含子树；与 EntityCommands.FindEntity 同语义）。</summary>
    public static GameEntity? FindEntity(IEnumerable<GameEntity>? entities, Guid guid)
    {
        if (entities == null) return null;
        foreach (var entity in entities)
        {
            if (entity.EntityGuid.Value == guid) return entity;
            var inChildren = FindEntity(entity.Children, guid);
            if (inChildren != null) return inChildren;
        }
        return null;
    }
}
