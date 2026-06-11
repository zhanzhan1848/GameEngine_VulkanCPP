using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// Maps editor GameEntity objects to native engine entity IDs.
/// Editor-side counterpart is purely dictionary lookups - the C++ engine owns the
/// actual entity data; the editor just tracks IDs so it can call RemoveGameEntity.
/// </summary>
public class EngineEntityMap
{
    private readonly Dictionary<ulong, GameEntity> _idToEntity = new();
    private readonly Dictionary<GameEntity, ulong> _entityToId = new();

    public int Count => _idToEntity.Count;

    public bool TryGetId(GameEntity entity, out ulong id) => _entityToId.TryGetValue(entity, out id);

    public bool TryGetEntity(ulong id, out GameEntity entity) => _idToEntity.TryGetValue(id, out entity);

    public void Register(GameEntity entity, ulong id)
    {
        if (entity == null) return;
        _idToEntity[id] = entity;
        _entityToId[entity] = id;
    }

    public void Unregister(ulong id)
    {
        if (_idToEntity.TryGetValue(id, out var entity))
        {
            _idToEntity.Remove(id);
            _entityToId.Remove(entity);
        }
    }

    public void Clear()
    {
        foreach (var id in _idToEntity.Keys)
        {
            NativeEngineBridge.RemoveGameEntity(id);
        }
        _idToEntity.Clear();
        _entityToId.Clear();
    }
}

/// <summary>
/// High-level synchronizer between editor GameEntity tree and the native engine.
/// Call SyncAll to push the current editor scene to the engine; call OnEditorChanged
/// incrementally to keep the engine in sync as the user edits.
/// </summary>
public static class EngineSync
{
    private static readonly EngineEntityMap _map = new();

    public static EngineEntityMap Map => _map;
    public static bool IsEngineActive => NativeEngineBridge.IsLoaded;

    public static void ClearEngineScene()
    {
        if (!IsEngineActive) return;
        _map.Clear();
        Logger.Info("Engine scene cleared");
    }

    public static ulong? PushEntity(GameEntity entity)
    {
        if (!IsEngineActive || entity == null) return null;
        var t = entity.GetTransform();
        if (t == null) return null;

        var id = NativeEngineBridge.CreateGameEntity(
            t.PositionX, t.PositionY, t.PositionZ,
            t.RotationX, t.RotationY, t.RotationZ,
            t.ScaleX, t.ScaleY, t.ScaleZ);

        if (id == 0) return null;
        _map.Register(entity, id);
        return id;
    }

    public static void PushScene(PrimalEditor_Avalonia.GameProject.Scene scene)
    {
        if (!IsEngineActive || scene == null) return;
        ClearEngineScene();
        foreach (var entity in scene.Entities)
        {
            PushRecursive(entity);
        }
        Logger.Info($"Pushed {_map.Count} entities to engine");
    }

    private static void PushRecursive(GameEntity entity)
    {
        PushEntity(entity);
        foreach (var child in entity.Children)
        {
            PushRecursive(child);
        }
    }
}

public static class GameEntityExtensions
{
    public static Transform? GetTransform(this GameEntity entity)
    {
        foreach (var c in entity.Components)
        {
            if (c is Transform t) return t;
        }
        return null;
    }
}