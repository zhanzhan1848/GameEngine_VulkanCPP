using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native;

public sealed class RuntimeMaterialMap
{
    private readonly Dictionary<AssetGuid, uint> _byGuid = new();

    public uint? GetRuntimeId(AssetGuid guid) =>
        _byGuid.TryGetValue(guid, out var id) ? id : null;

    public void Register(AssetGuid guid, uint runtimeId)
    {
        if (guid.Value == Guid.Empty) return;
        if (runtimeId == 0 || runtimeId == NativeEngineBridge.InvalidEntityId) return;
        _byGuid[guid] = runtimeId;
    }

    public bool Unregister(AssetGuid guid) => _byGuid.Remove(guid);

    public void Clear() => _byGuid.Clear();
}
