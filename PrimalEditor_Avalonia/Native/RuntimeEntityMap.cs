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
}
