using System;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>Read-only presentation of the MeshRenderer geometry reference.</summary>
public sealed class MeshAssetReferenceViewModel
{
    public AssetGuid MeshGuid { get; }
    public BuiltinMeshKind BuiltinMesh { get; }
    public AssetInfo? Asset { get; }
    public bool HasAsset => Asset != null;
    public bool HasGeometry => HasAsset || BuiltinMesh != BuiltinMeshKind.None;
    public string DisplayName { get; }
    public string PathDisplay { get; }
    public string GuidDisplay => MeshGuid.Value == Guid.Empty ? "—" : MeshGuid.Value.ToString("D");

    public MeshAssetReferenceViewModel(AssetGuid meshGuid, BuiltinMeshKind builtinMesh)
    {
        MeshGuid = meshGuid;
        BuiltinMesh = builtinMesh;
        Asset = meshGuid.Value == Guid.Empty ? null : AssetRegistry.GetByGuid(meshGuid.Value);

        if (Asset != null)
        {
            DisplayName = Asset.FileName;
            PathDisplay = AssetRegistry.GetRelativePath(Asset.FullPath)?.Value ?? Asset.FullPath;
        }
        else if (builtinMesh != BuiltinMeshKind.None)
        {
            DisplayName = $"Built-in {builtinMesh}";
            PathDisplay = "Procedural mesh";
        }
        else if (meshGuid.Value != Guid.Empty)
        {
            DisplayName = "Missing Mesh asset";
            PathDisplay = meshGuid.Value.ToString("D");
        }
        else
        {
            DisplayName = "Drop Mesh asset here";
            PathDisplay = "FBX, OBJ, glTF or Model asset";
        }
    }
}
