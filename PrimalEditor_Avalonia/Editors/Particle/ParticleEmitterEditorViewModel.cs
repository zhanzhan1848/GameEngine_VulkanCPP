using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Particles;
using PrimalEditor_Avalonia.Editors.Particle;
using NativeEngineBridge = PrimalEditor_Avalonia.Native.NativeEngineBridge;

namespace PrimalEditor_Avalonia.Editors.Particle;

/// <summary>
/// Asset-editor VM for .particle.json presets (AssetType.ParticlePreset),
/// hosted by AssetEditorHost exactly like the material editor:
/// left section palette / center over-life curve editor / right parameter
/// panel, with Save/Discard + status line owned by the host.
/// </summary>
public class ParticleEmitterEditorViewModel : AssetEditorViewModel
{
    public ParticlePresetDocument Document { get; private set; } = new();

    // ============================================================
    //  Left palette: sections (the particle analog of node groups)
    // ============================================================

    public IReadOnlyList<ParticleSectionTemplateViewModel> SectionPalette { get; } =
        new[]
        {
            new ParticleSectionTemplateViewModel("Metadata", "name / category / author"),
            new ParticleSectionTemplateViewModel("Emission", "spawn rate / burst / mode"),
            new ParticleSectionTemplateViewModel("Lifetime", "min / max"),
            new ParticleSectionTemplateViewModel("Velocity", "initial velocity range"),
            new ParticleSectionTemplateViewModel("Color", "start / end gradient"),
            new ParticleSectionTemplateViewModel("Scale", "size range / multiplier"),
            new ParticleSectionTemplateViewModel("Forces", "gravity / wind / drag"),
            new ParticleSectionTemplateViewModel("Rendering", "blend / depth"),
            new ParticleSectionTemplateViewModel("Texture", "sprite-sheet atlas"),
        };

    private int _selectedSectionIndex;
    public int SelectedSectionIndex
    {
        get => _selectedSectionIndex;
        set
        {
            if (_selectedSectionIndex == value) return;
            _selectedSectionIndex = Math.Clamp(value, 0, SectionPalette.Count - 1);
            OnPropertyChanged();
            OnPropertyChanged(nameof(SelectedSection));
            RebuildRows();
        }
    }

    public ParticleSectionTemplateViewModel? SelectedSection =>
        _selectedSectionIndex >= 0 && _selectedSectionIndex < SectionPalette.Count
            ? SectionPalette[_selectedSectionIndex]
            : null;

    public string SelectedSectionTitle => SelectedSection?.DisplayName?.ToUpperInvariant() ?? "PARAMETERS";

    private IReadOnlyList<ParticleParamViewModel> _rows = Array.Empty<ParticleParamViewModel>();
    /// <summary>Parameter rows of the selected section (right panel).</summary>
    public IReadOnlyList<ParticleParamViewModel> Rows
    {
        get => _rows;
        private set { _rows = value; OnPropertyChanged(); }
    }

    // ============================================================
    //  Center: over-life curves (6 slots, ParticleCurveSlot order)
    // ============================================================

    public IReadOnlyList<string> CurveSlotTitles =>
        Enum.GetValues<ParticleCurveSlot>().Select(ParticleCurveData.SlotTitle).ToArray();

    private int _selectedCurveSlot = (int)ParticleCurveSlot.Alpha;
    public int SelectedCurveSlot
    {
        get => _selectedCurveSlot;
        set
        {
            if (_selectedCurveSlot == value) return;
            _selectedCurveSlot = Math.Clamp(value, 0, 5);
            OnPropertyChanged();
            OnPropertyChanged(nameof(SelectedCurve));
            OnPropertyChanged(nameof(SelectedCurveEnabled));
            OnPropertyChanged(nameof(SelectedCurveInterpolation));
        }
    }

    /// <summary>Live curve model for the center editor (null = disabled).</summary>
    public ParticleCurveData? SelectedCurve => _curves[_selectedCurveSlot];

    public bool SelectedCurveEnabled
    {
        get => Document.IsCurveEnabled(_selectedCurveSlot);
        set
        {
            if (Document.IsCurveEnabled(_selectedCurveSlot) == value) return;
            Document.SetCurveEnabled(_selectedCurveSlot, value);
            if (value && _curves[_selectedCurveSlot] == null)
            {
                // Same default two-point curve the live-entity curve editor
                // creates (ParticleCurveEditorWindow.OnEnableChanged).
                var slot = (ParticleCurveSlot)_selectedCurveSlot;
                var (min, max) = ParticleCurveData.SlotValueRange(slot);
                float mid = (min + max) * 0.5f;
                var data = new ParticleCurveData { Slot = slot };
                data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { mid } });
                data.Keys.Add(new ParticleCurveKey { Time = 1f, Value = new[] { mid } });
                if (slot == ParticleCurveSlot.ColorGradient)
                {
                    data.Keys[0].Value = new[] { 1f, 1f, 1f, 1f };
                    data.Keys[1].Value = new[] { 1f, 1f, 1f, 1f };
                }
                _curves[_selectedCurveSlot] = data;
                OnPropertyChanged(nameof(SelectedCurve));
            }
            MarkDirty();
        }
    }

    public int SelectedCurveInterpolation
    {
        get => (int)(_curves[_selectedCurveSlot]?.Interpolation ?? ParticleCurveInterpolation.Linear);
        set
        {
            if (_curves[_selectedCurveSlot] == null) return;
            _curves[_selectedCurveSlot]!.Interpolation = (ParticleCurveInterpolation)value;
            MarkDirty();
        }
    }

    /// <summary>Called by the view's EmitterCurveEditor.CurveEdited — a drag on
    /// the center canvas finished, the live model already holds the new keys.</summary>
    public void NotifyCurveEdited() => MarkDirty();

    private readonly ParticleCurveData?[] _curves = new ParticleCurveData?[6];

    // ============================================================
    //  Dirty / status (same affordances as the material editor)
    // ============================================================

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; OnPropertyChanged(); OnPropertyChanged(nameof(DirtyMarker)); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;
    public string DirtyMarker => _isDirty ? "● unsaved" : string.Empty;

    public string StatusLine
    {
        get => _statusLine;
        private set { if (_statusLine != value) { _statusLine = value; OnPropertyChanged(); } }
    }
    private string _statusLine = "Ready";

    public override string Title => Asset != null ? $"Particle Emitter — {Asset.FileName}" : "Particle Emitter";

    public ParticleEmitterEditorViewModel()
    {
        RebuildRows();
    }

    // ============================================================
    //  Asset lifecycle
    // ============================================================

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        try
        {
            Document = ParticlePresetDocument.Read(info.FullPath);
        }
        catch (Exception ex)
        {
            Logger.Error($"ParticleEmitterEditorViewModel.SetAsset read failed: {ex.Message}\n{ex.StackTrace}", ex);
            Document = new ParticlePresetDocument
            {
                Name = Path.GetFileNameWithoutExtension(info.FileName.Replace(".particle", "")),
            };
        }
        LoadCurvesFromDocument();
        RebuildRows();
        ClearDirty();
        StatusLine = "Ready";
    }

    public override void Save()
    {
        if (Asset == null) return;
        try
        {
            PushCurvesToDocument();
            if (string.IsNullOrWhiteSpace(Document.Name))
                Document.Name = Path.GetFileNameWithoutExtension(Asset.FileName.Replace(".particle", ""));
            Document.Write(Asset.FullPath);

            // Re-register into the engine's preset manager so the Content
            // Browser "apply to entity" path and the Inspector combo see the
            // updated scalars without an app restart (追加语义 per EngineDLL).
            if (NativeEngineBridge.IsLoaded)
                NativeEngineBridge.ParticlePresetLoadFromFile(Asset.FullPath);

            ClearDirty();
            StatusLine = $"Saved {Asset.FileName}";
            Logger.Info($"Saved particle preset: {Asset.FileName}");
        }
        catch (Exception ex)
        {
            StatusLine = $"Save failed: {ex.Message}";
            Logger.Error($"Particle preset save failed: {ex.Message}", ex);
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        SetAsset(Asset);
        OnPropertyChanged(nameof(SelectedCurve));
        OnPropertyChanged(nameof(SelectedCurveEnabled));
        OnPropertyChanged(nameof(SelectedCurveInterpolation));
    }

    private void LoadCurvesFromDocument()
    {
        for (int slot = 0; slot < 6; ++slot)
        {
            _curves[slot] = null;
            var payload = Document.GetCurve(slot);
            if (payload == null || payload.Keys.Count == 0) continue;
            var slotEnum = (ParticleCurveSlot)slot;
            var data = new ParticleCurveData
            {
                Slot = slotEnum,
                Interpolation = (ParticleCurveInterpolation)Math.Clamp(payload.Interpolation, 0, 3),
            };
            int n = data.ValueCount;
            foreach (var k in payload.Keys)
            {
                var v = new float[n];
                for (int i = 0; i < n; ++i)
                    v[i] = i < k.Value.Length ? k.Value[i] : 0f;
                data.Keys.Add(new ParticleCurveKey
                {
                    Time = k.Time,
                    Value = v,
                    InTangent = k.InTangent,
                    OutTangent = k.OutTangent,
                });
            }
            _curves[slot] = data;
        }
    }

    private void PushCurvesToDocument()
    {
        for (int slot = 0; slot < 6; ++slot)
        {
            if (!Document.IsCurveEnabled(slot))
            {
                Document.SetCurve(slot, null);
                continue;
            }
            var data = _curves[slot];
            if (data == null || data.Keys.Count == 0) continue;
            var payload = new ParticlePresetDocument.CurvePayload
            {
                Interpolation = (int)data.Interpolation,
                Keys = data.Keys.Select(k => new ParticlePresetDocument.CurveKeyPayload
                {
                    Time = k.Time,
                    Value = (float[])k.Value.Clone(),
                    InTangent = k.InTangent,
                    OutTangent = k.OutTangent,
                }).ToList(),
            };
            Document.SetCurve(slot, payload);
        }
    }

    // ============================================================
    //  Parameter rows
    // ============================================================

    private void RebuildRows()
    {
        var c = Document.Config;
        List<ParticleParamViewModel> rows = _selectedSectionIndex switch
        {
            0 => new List<ParticleParamViewModel>
            {
                Text("Name", () => Document.Name, v => Document.Name = v),
                Text("Category", () => Document.Category, v => Document.Category = v),
                Text("Description", () => Document.Description, v => Document.Description = v),
                Text("Author", () => Document.Author, v => Document.Author = v),
                Int("Version", () => Document.Version, v => Document.Version = v),
            },
            1 => new List<ParticleParamViewModel>
            {
                Int("Max Particles", () => c.MaxParticles, v => c.MaxParticles = v),
                Float("Spawn Rate", () => c.SpawnRate, v => c.SpawnRate = v),
                Int("Burst Count", () => c.BurstCount, v => c.BurstCount = v),
                Bool("Ring Buffer", () => c.RingBufferMode, v => c.RingBufferMode = v),
                EnumRow("Mode", EmissionModeNames, () => c.Mode, v => c.Mode = v),
            },
            2 => new List<ParticleParamViewModel>
            {
                Float("Lifetime Min", () => c.LifetimeMin, v => c.LifetimeMin = v),
                Float("Lifetime Max", () => c.LifetimeMax, v => c.LifetimeMax = v),
            },
            3 => new List<ParticleParamViewModel>
            {
                Vec("Velocity Min", 3, () => c.VelocityMin, v => c.VelocityMin = v),
                Vec("Velocity Max", 3, () => c.VelocityMax, v => c.VelocityMax = v),
            },
            4 => new List<ParticleParamViewModel>
            {
                Vec("Color Start", 4, () => c.ColorStart, v => c.ColorStart = v),
                Vec("Color End", 4, () => c.ColorEnd, v => c.ColorEnd = v),
            },
            5 => new List<ParticleParamViewModel>
            {
                Vec("Scale Min", 2, () => c.ScaleMin, v => c.ScaleMin = v),
                Vec("Scale Max", 2, () => c.ScaleMax, v => c.ScaleMax = v),
                Float("Multiplier", () => c.ScaleMultiplier, v => c.ScaleMultiplier = v),
            },
            6 => new List<ParticleParamViewModel>
            {
                Vec("Gravity", 3, () => c.Gravity, v => c.Gravity = v),
                Vec("Wind", 3, () => c.Wind, v => c.Wind = v),
                Float("Drag", () => c.Drag, v => c.Drag = v),
            },
            7 => new List<ParticleParamViewModel>
            {
                EnumRow("Blend Mode", BlendModeNames, () => c.BlendMode, v => c.BlendMode = v),
                Bool("Depth Write", () => c.DepthWrite, v => c.DepthWrite = v),
            },
            8 => new List<ParticleParamViewModel>
            {
                Int("Atlas Columns", () => c.TextureAtlasColumns, v => c.TextureAtlasColumns = v),
                Int("Atlas Rows", () => c.TextureAtlasRows, v => c.TextureAtlasRows = v),
                Bool("Random Frame", () => c.TextureRandomFrame, v => c.TextureRandomFrame = v),
                Int("Frame Count", () => c.TextureFrameCount, v => c.TextureFrameCount = v),
            },
            _ => new List<ParticleParamViewModel>(),
        };
        Rows = rows;
        OnPropertyChanged(nameof(SelectedSectionTitle));
    }

    private static readonly string[] EmissionModeNames = { "Continuous", "Burst", "Ring Buffer" };
    private static readonly string[] BlendModeNames = { "Additive", "Alpha", "Multiply", "Premultiplied" };

    private static ParticleParamViewModel Float(string name, Func<float> get, Action<float> set) =>
        new(name, ParticleParamViewModel.ParamKind.Float, getFloat: get, setFloat: set);
    private static ParticleParamViewModel Int(string name, Func<int> get, Action<int> set) =>
        new(name, ParticleParamViewModel.ParamKind.Int, getInt: get, setInt: set);
    private static ParticleParamViewModel Bool(string name, Func<bool> get, Action<bool> set) =>
        new(name, ParticleParamViewModel.ParamKind.Bool, getBool: get, setBool: set);
    private static ParticleParamViewModel Text(string name, Func<string> get, Action<string> set) =>
        new(name, ParticleParamViewModel.ParamKind.Text, getText: get, setText: set);
    private static ParticleParamViewModel EnumRow(string name, string[] options, Func<int> get, Action<int> set) =>
        new(name, ParticleParamViewModel.ParamKind.Enum, getInt: get, setInt: set) { EnumOptions = options };
    private static ParticleParamViewModel Vec(string name, int components, Func<float[]> get, Action<float[]> set) =>
        new(name, ParticleParamViewModel.ParamKind.Vector, getVec: get, setVec: set) { ComponentCount = components };
}

public class ParticleSectionTemplateViewModel
{
    public string DisplayName { get; }
    public string Note { get; }
    public ParticleSectionTemplateViewModel(string displayName, string note)
    {
        DisplayName = displayName;
        Note = note;
    }
}
