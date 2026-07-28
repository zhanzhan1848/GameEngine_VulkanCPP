using System.IO;

namespace PrimalEditor_Avalonia.Content;

public interface IAssetImportSettings
{
    void ToBinary(BinaryWriter writer);
    void FromBinary(BinaryReader reader);
}

public static class AssetImportSettingsExtensions
{
    public static void CopyImportSettings<T>(T from, T to) where T : class, IAssetImportSettings
    {
        if (from == null || to == null)
            throw new System.ArgumentNullException("Arguments should not be null.");
        if (from.GetType() != to.GetType())
            throw new System.ArgumentException("Arguments should be of the same type.");

        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms)) { from.ToBinary(w); w.Flush(); }
        ms.Position = 0;
        using var r = new BinaryReader(ms);
        to.FromBinary(r);
    }
}