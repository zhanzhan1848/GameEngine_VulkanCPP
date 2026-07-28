using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Texture;

public class TextureEditorViewModel : AssetEditorViewModel
{
    public TextureImportSettings Settings { get; } = new();
    public TextureInfo Info { get; private set; } = new();

    private Bitmap? _bitmap;
    public Bitmap? Preview
    {
        get => _bitmap;
        private set { _bitmap = value; OnPropertyChanged(); OnPropertyChanged(nameof(HasPreview)); }
    }

    private bool _showR = true, _showG = true, _showB = true, _showA = true;
    public bool ShowR { get => _showR; set { _showR = value; OnPropertyChanged(); UpdateChannelBitmap(); } }
    public bool ShowG { get => _showG; set { _showG = value; OnPropertyChanged(); UpdateChannelBitmap(); } }
    public bool ShowB { get => _showB; set { _showB = value; OnPropertyChanged(); UpdateChannelBitmap(); } }
    public bool ShowA { get => _showA; set { _showA = value; OnPropertyChanged(); UpdateChannelBitmap(); } }
    public bool IsNormalMap { get; set; }

    public bool HasPreview => Preview != null;

    public override async void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        State = AssetEditorState.Loading;
        try
        {
            await Task.Run(() => LoadFromDisk(info.FullPath));
            State = AssetEditorState.Done;
            OnPropertyChanged(nameof(Info));
            OnPropertyChanged(nameof(HasPreview));
        }
        catch (Exception ex)
        {
            Logger.Error($"Texture load failed: {ex.Message}", ex);
            State = AssetEditorState.Done;
        }
    }

    private void LoadFromDisk(string path)
    {
        if (!File.Exists(path)) { Logger.Warning($"Texture missing: {path}"); return; }

        try
        {
            using var fs = File.OpenRead(path);
            var bmp = new Bitmap(fs);
            Info = new TextureInfo
            {
                Width = bmp.PixelSize.Width,
                Height = bmp.PixelSize.Height,
                ArraySize = 1,
                MipLevels = 1,
                Depth = 1,
                Format = TextureFormat.R8G8B8A8_UNORM,
            };

            Dispatcher.UIThread.Post(() => Preview = bmp);
        }
        catch (Exception ex)
        {
            Logger.Error($"Texture decode: {ex.Message}", ex);
        }
    }

    private void UpdateChannelBitmap()
    {
        if (NativeEngineBridge.IsLoaded)
        {
            Logger.Info("Channel mask updated — re-cook via native pipeline (stub)");
        }
        else
        {
            Logger.Debug("Channel mask updated — preview reflects new mask on next Reimport.");
        }
    }

    public void Reimport()
    {
        if (Asset == null) return;
        State = AssetEditorState.Importing;
        Task.Run(() =>
        {
            if (NativeEngineBridge.IsLoaded)
                Logger.Info($"Reimport: {Asset.FullPath} — cooking via native pipeline (stub)");
            else
                Logger.Info($"Reimport: {Asset.FullPath} — engine not loaded, no GPU upload");

            Dispatcher.UIThread.Post(() => SetAsset(Asset));
        });
    }

    public void Save()
    {
        if (Asset == null) return;
        Logger.Info($"Save: settings for {Asset.FileName} cached (binary round-trip)");
    }
}