using System;
using System.Collections.Generic;

namespace PrimalEditor_Avalonia.Content;

public enum AssetNodeKind
{
    Model,
    RootNode,
    MeshesFolder,
    Submesh,
    LODsFolder,
    LOD,
}

public sealed record AssetNode(
    AssetNodeKind Kind,
    string Name,
    int Index,
    IntPtr MeshHandle,
    IReadOnlyList<AssetNode>? Children = null)
{
    public bool IsContainer => Kind is AssetNodeKind.Model
        or AssetNodeKind.RootNode
        or AssetNodeKind.MeshesFolder
        or AssetNodeKind.LODsFolder;
}