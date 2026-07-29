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

public class TextureEditorViewModel : AssetEditorViewModel, IDirtyTrackable
{
    private ITextureSliceDecoder _decoder;
    public TextureImportSettings Settings { get; private set; } = new();
    public TextureInfo Info { get; private set; } = new();

    public TextureEditorViewModel() : this(new AvaloniaTextureSliceDecoder()) { }
    public TextureEditorViewModel(ITextureSliceDecoder decoder)
    {
        _decoder = decoder;
    }

    public void SetDecoder(ITextureSliceDecoder decoder) => _decoder = decoder;

    /// <summary>
    /// Test hook: install the given decoded slices and refresh all
    /// derived properties. Production code uses <see cref="SetAsset"/>
    /// which calls into the configured decoder; tests bypass file IO
    /// by supplying a <see cref="TextureDecoded"/> directly.
    /// </summary>
    public void SetSlicesForTest(TextureDecoded? decoded)
    {
        if (decoded == null)
        {
            _slices = null;
        }
        else
        {
            _slices = new TextureSliceSet(decoded.Width, decoded.Height, decoded.ArrayCount, decoded.MipCount, decoded.Rgba);
        }
        _mipIndex = Clamp(_mipIndex, 0, MaxMipIndex);
        _arrayIndex = Clamp(_arrayIndex, 0, MaxArrayIndex);
        _depthIndex = Clamp(_depthIndex, 0, MaxDepthIndex);
        if (decoded != null)
        {
            Info = new TextureInfo
            {
                Width = decoded.Width,
                Height = decoded.Height,
                ArraySize = Math.Max(decoded.ArrayCount, Settings.ArraySize),
                MipLevels = Math.Max(decoded.MipCount, Settings.MipLevels),
                Depth = Math.Max(decoded.Depth, Settings.Depth),
                Format = Settings.OutputFormat,
            };
        }
        OnPropertyChanged(nameof(MaxMipIndex));
        OnPropertyChanged(nameof(MaxArrayIndex));
        OnPropertyChanged(nameof(MaxDepthIndex));
        OnPropertyChanged(nameof(MipDisplay));
        OnPropertyChanged(nameof(ArrayDisplay));
        OnPropertyChanged(nameof(DepthDisplay));
        OnPropertyChanged(nameof(Info));
    }

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; OnPropertyChanged(); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;

    private Bitmap? _bitmap;
    public Bitmap? Preview
    {
        get => _bitmap;
        private set { _bitmap = value; OnPropertyChanged(); OnPropertyChanged(nameof(HasPreview)); }
    }
    public bool HasPreview => Preview != null;

    public bool ShowR { get => Settings.Compress ? _showR : true; set { _showR = value; OnPropertyChanged(); OnPropertyChanged(nameof(ShowG)); RebuildPreview(); } }
    public bool ShowG { get => _showG; set { _showG = value; OnPropertyChanged(); RebuildPreview(); } }
    public bool ShowB { get => _showB; set { _showB = value; OnPropertyChanged(); RebuildPreview(); } }
    public bool ShowA { get => _showA; set { _showA = value; OnPropertyChanged(); RebuildPreview(); } }
    private bool _showR = true, _showG = true, _showB = true, _showA = true;

    public bool IsNormalMap
    {
        get => Settings.IsNormalMap;
        set
        {
            if (Settings.IsNormalMap == value) return;
            var prev = Settings.IsNormalMap;
            Settings.IsNormalMap = value;
            RecordChange(new DelegateAction(
                "Toggle Normal Map",
                () => { Settings.IsNormalMap = prev; OnPropertyChanged(); MarkDirty(); RebuildPreview(); },
                () => { Settings.IsNormalMap = value; OnPropertyChanged(); MarkDirty(); RebuildPreview(); }));
            OnPropertyChanged(); MarkDirty(); RebuildPreview();
        }
    }
    public bool IsSRGB
    {
        get => Settings.IsSRGB;
        set
        {
            if (Settings.IsSRGB == value) return;
            var prev = Settings.IsSRGB;
            Settings.IsSRGB = value;
            RecordChange(new DelegateAction(
                "Toggle sRGB",
                () => { Settings.IsSRGB = prev; OnPropertyChanged(); MarkDirty(); },
                () => { Settings.IsSRGB = value; OnPropertyChanged(); MarkDirty(); }));
            OnPropertyChanged(); MarkDirty();
        }
    }
    public TextureFormat OutputFormat
    {
        get => Settings.OutputFormat;
        set
        {
            if (Settings.OutputFormat == value) return;
            var prev = Settings.OutputFormat;
            Settings.OutputFormat = value;
            RecordChange(new DelegateAction(
                $"Set OutputFormat={value}",
                () => { Settings.OutputFormat = prev; OnPropertyChanged(); MarkDirty(); },
                () => { Settings.OutputFormat = value; OnPropertyChanged(); MarkDirty(); }));
            OnPropertyChanged(); MarkDirty();
        }
    }
    public IReadOnlyList<TextureFormat> AllFormats { get; } = (TextureFormat[])Enum.GetValues(typeof(TextureFormat));

    private TextureSliceSet? _slices;

    private int _mipIndex;
    public int MipIndex
    {
        get => _mipIndex;
        set
        {
            var v = Clamp(value, 0, MaxMipIndex);
            if (_mipIndex != v)
            {
                _mipIndex = v;
                OnPropertyChanged();
                OnPropertyChanged(nameof(MipDisplay));
            }
        }
    }
    public int MaxMipIndex => _slices?.MipCount > 0 ? _slices.MipCount - 1 : 0;
    public string MipDisplay => $"Mip: {_mipIndex}/{MaxMipIndex}";

    private int _arrayIndex;
    public int ArrayIndex
    {
        get => _arrayIndex;
        set
        {
            var v = Clamp(value, 0, MaxArrayIndex);
            if (_arrayIndex != v) { _arrayIndex = v; OnPropertyChanged(); OnPropertyChanged(nameof(ArrayDisplay)); }
        }
    }
    public int MaxArrayIndex => _slices?.ArrayCount > 0 ? _slices.ArrayCount - 1 : 0;
    public string ArrayDisplay => $"Array: {_arrayIndex}/{MaxArrayIndex}";

    private int _depthIndex;
    public int DepthIndex
    {
        get => _depthIndex;
        set
        {
            var v = Clamp(value, 0, MaxDepthIndex);
            if (_depthIndex != v) { _depthIndex = v; OnPropertyChanged(); OnPropertyChanged(nameof(DepthDisplay)); }
        }
    }
    public int MaxDepthIndex => _slices?.Depth > 0 ? _slices.Depth - 1 : 0;
    public string DepthDisplay => $"Depth: {_depthIndex}/{MaxDepthIndex}";

    private static int Clamp(int value, int min, int max) => Math.Max(min, Math.Min(max, value));

    public bool CanReimport => Asset != null && !IsBusy;
    public bool CanSave => Asset != null && IsDirty && !IsBusy;

    private bool IsBusy =>
        State == AssetEditorState.Loading
     || State == AssetEditorState.Importing
     || State == AssetEditorState.Processing
     || State == AssetEditorState.Saving;

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        Settings = AssetImportSettingsStore.Load<TextureImportSettings>(info.FullPath);
        ClearDirty();
        _mipIndex = 0; _arrayIndex = 0; _depthIndex = 0;
        State = AssetEditorState.Loading;
        _ = LoadAsync(info.FullPath);
    }

    private async Task LoadAsync(string path)
    {
        try
        {
            await Task.Run(() => LoadFromDisk(path));
            State = AssetEditorState.Done;
            OnPropertyChanged(nameof(Info));
            OnPropertyChanged(nameof(CanReimport));
            OnPropertyChanged(nameof(CanSave));
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
        TextureDecoded? decoded;
        try
        {
            decoded = _decoder.TryDecode(path);
        }
        catch (Exception ex)
        {
            Logger.Error($"Texture decode: {ex.Message}", ex);
            return;
        }
        if (decoded == null) return;
        _slices = new TextureSliceSet(decoded.Width, decoded.Height, decoded.ArrayCount, decoded.MipCount, decoded.Rgba);
        OnPropertyChanged(nameof(MaxMipIndex));
        OnPropertyChanged(nameof(MaxArrayIndex));
        OnPropertyChanged(nameof(MaxDepthIndex));
        OnPropertyChanged(nameof(MipDisplay));
        OnPropertyChanged(nameof(ArrayDisplay));
        OnPropertyChanged(nameof(DepthDisplay));
        Info = new TextureInfo
        {
            Width = decoded.Width,
            Height = decoded.Height,
            ArraySize = Math.Max(decoded.ArrayCount, Settings.ArraySize),
            MipLevels = Math.Max(decoded.MipCount, Settings.MipLevels),
            Depth = Math.Max(decoded.Depth, Settings.Depth),
            Format = Settings.OutputFormat,
        };
        Dispatcher.UIThread.Post(RebuildPreview);
    }

    private void RebuildPreview()
    {
        if (_slices == null) return;
        var src = _slices.Rgba.Span;
        var bytes = new byte[src.Length];
        float r = _showR ? 1f : 0f;
        float g = _showG ? 1f : 0f;
        float b = _showB ? 1f : 0f;
        float a = _showA ? 1f : 0f;
        bool normal = Settings.IsNormalMap;
        for (int i = 0; i < bytes.Length; i += 4)
        {
            byte sr = src[i + 0], sg = src[i + 1], sb = src[i + 2], sa = src[i + 3];
            if (normal)
            {
                sr = (byte)Math.Clamp(sr * 2 - 255, 0, 255);
                sg = (byte)Math.Clamp(sg * 2 - 255, 0, 255);
                sb = (byte)Math.Clamp(sb * 2 - 255, 0, 255);
            }
            bytes[i + 0] = (byte)(sr * r);
            bytes[i + 1] = (byte)(sg * g);
            bytes[i + 2] = (byte)(sb * b);
            bytes[i + 3] = (byte)(sa * a);
        }
        var handle = System.Runtime.InteropServices.GCHandle.Alloc(bytes, System.Runtime.InteropServices.GCHandleType.Pinned);
        try
        {
            Preview = new WriteableBitmap(
                Avalonia.Platform.PixelFormat.Rgba8888,
                Avalonia.Platform.AlphaFormat.Premul,
                handle.AddrOfPinnedObject(),
                new Avalonia.PixelSize(_slices.Width, _slices.Height),
                new Avalonia.Vector(96, 96),
                _slices.Width * 4);
        }
        finally
        {
            handle.Free();
        }
    }

    public void Reimport()
    {
        if (Asset == null || !CanReimport) return;
        State = AssetEditorState.Importing;
        OnPropertyChanged(nameof(CanReimport));
        OnPropertyChanged(nameof(CanSave));
        var path = Asset.FullPath;
        var task = Task.Run(() =>
        {
            if (NativeEngineBridge.IsLoaded)
            {
                var result = NativeEngineBridge.ImportTexture(path);
                if (result.HasValue)
                {
                    var info = result.Value;
                    Logger.Info(info.Success != 0
                        ? $"Reimport cook ok: {path} ({info.Width}x{info.Height})"
                        : $"Reimport cook failed: {path} — {info.ErrorMessage}");
                }
                else
                {
                    Logger.Warning($"Reimport: {path} — engine call returned no result");
                }
            }
            else
            {
                Logger.Info($"Reimport: {path} — engine not loaded, settings-only round trip");
            }
            AssetImportSettingsStore.Save(path, Settings);
            Dispatcher.UIThread.Post(() =>
            {
                ClearDirty();
                State = AssetEditorState.Done;
                OnPropertyChanged(nameof(CanReimport));
                OnPropertyChanged(nameof(CanSave));
            });
        });
    }

    public override void Save()
    {
        if (Asset == null || !CanSave) return;
        var ok = AssetImportSettingsStore.Save(Asset.FullPath, Settings);
        if (ok)
        {
            ClearDirty();
            OnPropertyChanged(nameof(CanSave));
            Logger.Info($"Saved texture settings: {Asset.FileName}");
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        Settings = AssetImportSettingsStore.Load<TextureImportSettings>(Asset.FullPath);
        ClearDirty();
        RebuildPreview();
        OnPropertyChanged(nameof(IsNormalMap));
        OnPropertyChanged(nameof(IsSRGB));
        OnPropertyChanged(nameof(OutputFormat));
        OnPropertyChanged(nameof(CanSave));
    }
}
