using System.IO;
using System.Text.Json.Nodes;

namespace PrimalEditor_Avalonia.Content;

public interface IAssetImportSettings
{
    void ToBinary(BinaryWriter writer);
    void FromBinary(BinaryReader reader);
    void FromJson(JsonObject settings);
    JsonObject ToJson();
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
        using (var w = new BinaryWriter(ms, System.Text.Encoding.UTF8, leaveOpen: true)) { from.ToBinary(w); w.Flush(); }
        ms.Position = 0;
        using var r = new BinaryReader(ms, System.Text.Encoding.UTF8, leaveOpen: true);
        to.FromBinary(r);
    }
}