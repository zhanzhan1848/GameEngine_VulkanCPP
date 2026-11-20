using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.Native;

public sealed class RuntimeEntityMap
{
    private readonly Dictionary<EntityGuid, uint> _byGuid = new();

    public uint? GetEntityId(EntityGuid guid) =>
        _byGuid.TryGetValue(guid, out var id) ? id : null;

    public void Register(EntityGuid guid, uint entityId)
    {
        if (guid.Value == Guid.Empty) return;
        if (entityId == 0 || entityId == NativeEngineBridge.InvalidEntityId) return;
        _byGuid[guid] = entityId;
    }

    public bool Unregister(EntityGuid guid) => _byGuid.Remove(guid);

    public void Clear() => _byGuid.Clear();

    /// <summary>
    /// 当前全部 guid → runtime id 映射的只读枚举（2026-11-05 lightmap 稳定键
    /// 计划：全量快照推送遍历用）。调用方不得修改返回的键值。
    /// </summary>
    public IEnumerable<KeyValuePair<EntityGuid, uint>> Enumerate() => _byGuid;
}
