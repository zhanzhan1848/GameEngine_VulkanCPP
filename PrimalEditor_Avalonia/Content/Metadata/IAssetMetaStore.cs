namespace PrimalEditor_Avalonia.Content.Metadata;

public interface IAssetMetaStore
{
    AssetMetaDocument? Read(string assetPath);
    bool Write(string assetPath, AssetMetaDocument document);
}