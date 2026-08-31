using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native;

public sealed class RuntimeAssetMap
{
    private readonly Dictionary<AssetGuid, ulong> _byGuid = new();

    public ulong? GetContentId(AssetGuid guid) =>
        _byGuid.TryGetValue(guid, out var id) ? id : null;

    public void Register(AssetGuid guid, ulong contentId)
    {
        if (guid.Value == Guid.Empty) return;
		// Native content slots are FreeList-backed, so zero is the first valid
		// geometry id. Only invalid_id (0xffffffff) represents failure.
		if (contentId == NativeEngineBridge.InvalidContentId) return;
        _byGuid[guid] = contentId;
    }

    public bool Unregister(AssetGuid guid) => _byGuid.Remove(guid);

    public void Clear() => _byGuid.Clear();
}
