using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;

namespace PrimalEditor_Avalonia.Editors.Material;

public class MaterialEditorViewModel : AssetEditorViewModel, IDirtyTrackable
{
    public MaterialDocument Document { get; private set; } = new();

    public string Name
    {
        get => Document.Name;
        set
        {
            if (Document.Name == value) return;
            Document.Name = value ?? string.Empty;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public AssetGuid ShaderGuid
    {
        get => Document.ShaderGuid;
        set
        {
            if (Document.ShaderGuid == value) return;
            Document.ShaderGuid = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    /// <summary>
    /// Textures slot lookup for XAML: { slot name → AssetGuid string }.
    /// Bindings go through this dictionary so the inspector can iterate
    /// arbitrary slots without baking names into the schema.
    /// </summary>
    public IDictionary<string, string> TextureSlots
    {
        get => _textureSlotView;
        private set { _textureSlotView = value; OnPropertyChanged(); }
    }
    private IDictionary<string, string> _textureSlotView = new Dictionary<string, string>();

    public string Surface
    {
        get => Document.RenderState.Surface;
        set
        {
            if (Document.RenderState.Surface == value) return;
            Document.RenderState.Surface = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public string CullMode
    {
        get => Document.RenderState.CullMode;
        set
        {
            if (Document.RenderState.CullMode == value) return;
            Document.RenderState.CullMode = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public bool DepthTest
    {
        get => Document.RenderState.DepthTest;
        set { Document.RenderState.DepthTest = value; OnPropertyChanged(); MarkDirty(); }
    }

    public bool DepthWrite
    {
        get => Document.RenderState.DepthWrite;
        set { Document.RenderState.DepthWrite = value; OnPropertyChanged(); MarkDirty(); }
    }

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; OnPropertyChanged(); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        Document = MaterialSerializer.Read(info.FullPath) ?? new MaterialDocument
        {
            Name = Path.GetFileNameWithoutExtension(info.FileName),
        };
        SyncTextureSlots();
        ClearDirty();
    }

    public override void Save()
    {
        if (Asset == null) return;
        try
        {
            MaterialSerializer.Write(Asset.FullPath, Document);
            ClearDirty();
            Logger.Info($"Saved material: {Asset.FileName}");
        }
        catch (Exception ex)
        {
            Logger.Error($"Material save failed: {ex.Message}", ex);
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        SetAsset(Asset);
        ClearDirty();
    }

    /// <summary>
    /// Update <see cref="TextureSlots"/> whenever the underlying
    /// dictionary changes. XAML binds to a flat dict for simplicity;
    /// the editor's "Add/Remove slot" UI calls this after mutation.
    /// </summary>
    public void SyncTextureSlots()
    {
        TextureSlots = Document.Textures.ToDictionary(kv => kv.Key, kv => kv.Value.Value.ToString("D"));
    }
}
