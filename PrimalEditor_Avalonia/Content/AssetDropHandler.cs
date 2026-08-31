using System.IO;
using System.Threading.Tasks;
using Avalonia.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Shared drop-target plumbing for editor panels that accept assets from
/// the Content Browser. See <see cref="RunAsync"/> for the canonical entry
/// point used by every drop target.
/// </summary>
public static class AssetDropHandler
{
    public static AssetDragPayload? ReadPayload(DragEventArgs e)
    {
        var format = DataFormat.CreateStringApplicationFormat(AssetDragPayload.DataFormat);
        if (!e.DataTransfer.Contains(format)) return null;
        var raw = e.DataTransfer.TryGetValue(format);
        return AssetDragPayload.Deserialize(raw ?? string.Empty);
    }

    public static string? FirstFilePath(DragEventArgs e)
    {
        if (!e.DataTransfer.Contains(DataFormat.File)) return null;
        var items = e.DataTransfer.TryGetFiles();
        if (items == null) return null;
        foreach (var item in items)
        {
            if (item is Avalonia.Platform.Storage.IStorageItem storage && storage.Path.IsFile)
            {
                return storage.Path.LocalPath;
            }
        }
        return null;
    }

    /// <summary>
    /// Resolve a payload GUID into an <see cref="AssetInfo"/> from the
    /// registry; falls back to the parent GUID for synthetic submesh
    /// children, then synthesises a minimal record as a last resort.
    /// </summary>
    public static AssetInfo? Resolve(AssetDragPayload payload)
    {
        var info = AssetRegistry.GetByGuid(payload.Guid);
        if (info != null) return info;
        if (payload.ParentGuid.HasValue)
        {
            var parent = AssetRegistry.GetByGuid(payload.ParentGuid.Value);
            if (parent != null) return parent;
        }
        return new AssetInfo
        {
            Guid = payload.Guid,
            FullPath = payload.FullPath,
            Type = payload.IsContainer ? AssetType.Model : AssetType.Unknown,
        };
    }

    /// <summary>
    /// Choose the cursor effect for <see cref="DragDrop.DragOverEvent"/>:
    /// <see cref="DragDropEffects.Copy"/> only when the scene is active and
    /// the drag contains a recognised payload.
    /// </summary>
    public static DragDropEffects EffectsFor(DragEventArgs e, WorldEditorViewModel? vm)
    {
        if (vm?.ActiveScene == null) return DragDropEffects.None;
        if (ReadPayload(e) != null) return DragDropEffects.Copy;
        if (FirstFilePath(e) != null) return DragDropEffects.Copy;
        return DragDropEffects.None;
    }

    /// <summary>
    /// Resolve the dropped data into an <see cref="AssetInfo"/> and feed it
    /// to <see cref="WorldEditorViewModel.DropAssetAsync"/>. Returns false
    /// when nothing droppable was found.
    /// </summary>
    public static async Task<bool> RunAsync(DragEventArgs e, WorldEditorViewModel? vm, float posX, float posY, float posZ)
    {
        if (vm == null)
        {
            Logger.Warning("AssetDropHandler: vm is null");
            return false;
        }
        if (vm.ActiveScene == null)
        {
            Logger.Warning("AssetDropHandler: no active scene");
            return false;
        }

        var payload = ReadPayload(e);
        AssetInfo? info = payload != null ? Resolve(payload) : null;

        if (info == null)
        {
            var droppedPath = FirstFilePath(e);
            if (string.IsNullOrEmpty(droppedPath) || !File.Exists(droppedPath))
            {
                Logger.Warning("AssetDropHandler: no asset info from drag (no payload, no file path)");
                return false;
            }
            info = AssetRegistry.Get(droppedPath) ?? new AssetInfo
            {
                FullPath = droppedPath,
                Type = ContentHelper.GetAssetType(droppedPath),
            };
        }

        Logger.Info($"AssetDropHandler: invoking DropAssetAsync for {info.FileName} type={info.Type}");
        try
        {
            return await vm.DropAssetAsync(info, posX, posY, posZ) != null;
        }
        catch (System.Exception ex)
        {
            Logger.Error("DropAsset failed", ex);
            return false;
        }
    }
}
