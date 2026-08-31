using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Shaders;

namespace PrimalEditor_Avalonia.Editors.Shaders;

public class ShaderEditorViewModel : AssetEditorViewModel, IDirtyTrackable
{
    public ShaderDocument Document { get; private set; } = new();

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

    /// <summary>Lookup for the XAML language-variant picker
    /// ({ backend name → source file path }).</summary>
    public IDictionary<string, string> Variants
    {
        get => _variantView;
        private set { _variantView = value; OnPropertyChanged(); }
    }
    private IDictionary<string, string> _variantView = new Dictionary<string, string>();

    /// <summary>Entry-point lookup ({ "Vertex"/"Fragment" → symbol name }).</summary>
    public IDictionary<string, string> EntryPoints
    {
        get => _entryPointView;
        private set { _entryPointView = value; OnPropertyChanged(); }
    }
    private IDictionary<string, string> _entryPointView = new Dictionary<string, string>();

    public string VertexEntry
    {
        get => Document.EntryPoints.TryGetValue("Vertex", out var v) ? v : string.Empty;
        set { SetEntryPoint("Vertex", value); }
    }
    public string FragmentEntry
    {
        get => Document.EntryPoints.TryGetValue("Fragment", out var v) ? v : string.Empty;
        set { SetEntryPoint("Fragment", value); }
    }

    public string MetalSource
    {
        get => Document.LanguageVariants.TryGetValue("Metal", out var v) ? v : string.Empty;
        set { SetVariant("Metal", value); }
    }
    public string HlslSource
    {
        get => Document.LanguageVariants.TryGetValue("HLSL", out var v) ? v : string.Empty;
        set { SetVariant("HLSL", value); }
    }
    public string GlslSource
    {
        get => Document.LanguageVariants.TryGetValue("GLSL", out var v) ? v : string.Empty;
        set { SetVariant("GLSL", value); }
    }

    public string TechniquesCsv
    {
        get => string.Join(",", Document.Techniques);
        set
        {
            Document.Techniques = (value ?? string.Empty)
                .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .ToList();
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public string KeywordsCsv
    {
        get => string.Join(",", Document.Keywords);
        set
        {
            Document.Keywords = (value ?? string.Empty)
                .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .ToList();
            OnPropertyChanged();
            MarkDirty();
        }
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
        Document = ShaderSerializer.Read(info.FullPath) ?? new ShaderDocument
        {
            Name = Path.GetFileNameWithoutExtension(info.FileName),
            EntryPoints = new System.Collections.Generic.Dictionary<string, string>
            {
                ["Vertex"] = "VSMain",
                ["Fragment"] = "PSMain",
            },
        };
        SyncViews();
        ClearDirty();
    }

    public override void Save()
    {
        if (Asset == null) return;
        try
        {
            ShaderSerializer.Write(Asset.FullPath, Document);
            ClearDirty();
            Logger.Info($"Saved shader: {Asset.FileName}");
        }
        catch (Exception ex)
        {
            Logger.Error($"Shader save failed: {ex.Message}", ex);
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        SetAsset(Asset);
        ClearDirty();
    }

    private void SetVariant(string lang, string sourcePath)
    {
        var value = sourcePath ?? string.Empty;
        if (Document.LanguageVariants.TryGetValue(lang, out var existing) && existing == value) return;
        Document.LanguageVariants[lang] = value;
        SyncViews();
        MarkDirty();
    }

    private void SetEntryPoint(string stage, string symbol)
    {
        var value = symbol ?? string.Empty;
        if (Document.EntryPoints.TryGetValue(stage, out var existing) && existing == value) return;
        Document.EntryPoints[stage] = value;
        SyncViews();
        MarkDirty();
    }

    private void SyncViews()
    {
        Variants = new Dictionary<string, string>(Document.LanguageVariants);
        EntryPoints = new Dictionary<string, string>(Document.EntryPoints);
    }
}
