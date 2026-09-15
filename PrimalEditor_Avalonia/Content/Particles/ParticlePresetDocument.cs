using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content.Particles;

/// <summary>
/// C# mirror of the engine's <c>particle_preset</c> JSON layout
/// (Engine/Particles/ParticlePreset.cpp <c>preset_json::to_json</c>).
/// Property names are the engine's exact keys — the engine's hand-rolled
/// <c>from_json</c> does a raw substring search for <c>"key":</c>, so the
/// serializer must stay compact (no spaces) and must not rename fields.
///
/// Curve keyframes are a C#-side extension (engine to_json only persists the
/// six use_* flags). They round-trip through files written by this editor;
/// the engine's parser ignores the extra keys.
/// </summary>
public sealed class ParticlePresetDocument
{
    // ---- Metadata ----------------------------------------------------------

    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("category")] public string Category { get; set; } = "Effects";
    [JsonPropertyName("description")] public string Description { get; set; } = "";
    [JsonPropertyName("author")] public string Author { get; set; } = "";
    [JsonPropertyName("version")] public int Version { get; set; } = 1;

    // ---- Emitter config ------------------------------------------------------

    [JsonPropertyName("config")] public EmitterConfig Config { get; set; } = new();

    // ---- Curve flags (engine keys) ------------------------------------------

    [JsonPropertyName("use_scale_curve")] public bool UseScaleCurve { get; set; }
    [JsonPropertyName("use_alpha_curve")] public bool UseAlphaCurve { get; set; }
    [JsonPropertyName("use_color_gradient")] public bool UseColorGradient { get; set; }
    [JsonPropertyName("use_velocity_curve")] public bool UseVelocityCurve { get; set; }
    [JsonPropertyName("use_force_curve")] public bool UseForceCurve { get; set; }
    [JsonPropertyName("use_rotation_curve")] public bool UseRotationCurve { get; set; }

    // ---- Curve keyframes (C#-side extension; engine ignores on load) ---------

    [JsonPropertyName("scale_curve")] public CurvePayload? ScaleCurve { get; set; }
    [JsonPropertyName("alpha_curve")] public CurvePayload? AlphaCurve { get; set; }
    [JsonPropertyName("color_gradient_curve")] public CurvePayload? ColorGradientCurve { get; set; }
    [JsonPropertyName("velocity_curve")] public CurvePayload? VelocityCurve { get; set; }
    [JsonPropertyName("force_curve")] public CurvePayload? ForceCurve { get; set; }
    [JsonPropertyName("rotation_curve")] public CurvePayload? RotationCurve { get; set; }

    public sealed class EmitterConfig
    {
        [JsonPropertyName("max_particles")] public int MaxParticles { get; set; } = 1000;
        [JsonPropertyName("spawn_rate")] public float SpawnRate { get; set; } = 100f;
        [JsonPropertyName("burst_count")] public int BurstCount { get; set; }
        [JsonPropertyName("ring_buffer_mode")] public bool RingBufferMode { get; set; } = true;
        [JsonPropertyName("mode")] public int Mode { get; set; } // emission_mode: 0 continuous, 1 burst, 2 ring_buffer

        [JsonPropertyName("lifetime_min")] public float LifetimeMin { get; set; } = 1f;
        [JsonPropertyName("lifetime_max")] public float LifetimeMax { get; set; } = 3f;

        [JsonPropertyName("velocity_min")] public float[] VelocityMin { get; set; } = { 0f, 1f, 0f };
        [JsonPropertyName("velocity_max")] public float[] VelocityMax { get; set; } = { 0f, 5f, 0f };

        [JsonPropertyName("color_start")] public float[] ColorStart { get; set; } = { 1f, 1f, 1f, 1f };
        [JsonPropertyName("color_end")] public float[] ColorEnd { get; set; } = { 0.5f, 0.5f, 1f, 0.8f };

        [JsonPropertyName("scale_min")] public float[] ScaleMin { get; set; } = { 0.1f, 0.1f };
        [JsonPropertyName("scale_max")] public float[] ScaleMax { get; set; } = { 1f, 1f };
        [JsonPropertyName("scale_multiplier")] public float ScaleMultiplier { get; set; } = 1f;

        [JsonPropertyName("gravity")] public float[] Gravity { get; set; } = { 0f, -9.8f, 0f };
        [JsonPropertyName("wind")] public float[] Wind { get; set; } = { 0f, 0f, 0f };
        [JsonPropertyName("drag")] public float Drag { get; set; }

        [JsonPropertyName("blend_mode")] public int BlendMode { get; set; } // blend_mode: 0 additive, 1 alpha, 2 multiply, 3 premultiplied
        [JsonPropertyName("depth_write")] public bool DepthWrite { get; set; }

        [JsonPropertyName("texture_atlas_columns")] public int TextureAtlasColumns { get; set; } = 1;
        [JsonPropertyName("texture_atlas_rows")] public int TextureAtlasRows { get; set; } = 1;
        [JsonPropertyName("texture_random_frame")] public bool TextureRandomFrame { get; set; }
        [JsonPropertyName("texture_frame_count")] public int TextureFrameCount { get; set; } = 1;
    }

    public sealed class CurvePayload
    {
        [JsonPropertyName("interpolation")] public int Interpolation { get; set; } = 1;
        [JsonPropertyName("keys")] public List<CurveKeyPayload> Keys { get; set; } = new();
    }

    public sealed class CurveKeyPayload
    {
        [JsonPropertyName("t")] public float Time { get; set; }
        [JsonPropertyName("v")] public float[] Value { get; set; } = { 0f };
        [JsonPropertyName("inTangent")] public float InTangent { get; set; }
        [JsonPropertyName("outTangent")] public float OutTangent { get; set; }
    }

    // ---- (de)serialization ----------------------------------------------------

    private static readonly JsonSerializerOptions Options = new()
    {
        // Engine keys are lower_snake_case and matched verbatim; stay
        // case-sensitive so typos surface as defaults instead of silent remaps.
        PropertyNameCaseInsensitive = false,
        WriteIndented = false, // engine's naive parser stops at whitespace-safe delimiters either way; compact keeps "key":value searchable
    };

    public static ParticlePresetDocument Read(string path)
    {
        var json = File.ReadAllText(path);
        try
        {
            return Deserialize(json) ?? throw new JsonException("null document");
        }
        catch (JsonException)
        {
            // Presets exported before the engine json_writer comma fix contain
            // arrays like [-0.2-0.1-0.2] (missing element separators). Repair
            // in-memory so legacy files load; Save rewrites them validly.
            var repaired = RepairMissingArrayCommas(json);
            return Deserialize(repaired) ?? throw new JsonException("unparseable preset (even after repair)");
        }
    }

    private static ParticlePresetDocument? Deserialize(string json) =>
        JsonSerializer.Deserialize<ParticlePresetDocument>(json, Options);

    /// <summary>Inside [ ] spans, insert the commas the old engine writer
    /// omitted between adjacent numeric literals. The engine format is fully
    /// determined: the key fixes the element count (velocity 3, colors 4,
    /// scale 2, …) and values are minimal decimal literals — so we backtrack
    /// split the span into exactly <c>arity</c> valid JSON numbers (longest
    /// candidate first) instead of guessing at character level.</summary>
    public static string RepairMissingArrayCommas(string json)
    {
        var sb = new System.Text.StringBuilder(json.Length + 64);
        string lastKey = "";
        for (int i = 0; i < json.Length; )
        {
            char c = json[i];
            if (c == '"')
            {
                // Consume the whole string (escape-aware so \" doesn't end it).
                int close = i + 1;
                while (close < json.Length)
                {
                    if (json[close] == '\\') { close += 2; continue; }
                    if (json[close] == '"') break;
                    ++close;
                }
                if (close >= json.Length) { sb.Append(json, i, json.Length - i); break; }
                var text = json.Substring(i + 1, close - i - 1);
                int colon = close + 1;
                while (colon < json.Length && char.IsWhiteSpace(json[colon])) ++colon;
                if (colon < json.Length && json[colon] == ':') lastKey = text;
                sb.Append(json, i, close - i + 1);
                i = close + 1;
                continue;
            }
            if (c == '[' && ArrayArities.TryGetValue(lastKey, out var arity)
                && TryEmitSplitArray(json, i, arity, sb, out var next))
            {
                i = next;
                continue;
            }
            sb.Append(c);
            ++i;
        }
        return sb.ToString();
    }

    /// <summary>Emit <c>json[start..]</c>'s array content split into exactly
    /// <c>arity</c> comma-separated numbers. Returns false when the span does
    /// not admit such a split (caller falls back to verbatim copy).</summary>
    private static bool TryEmitSplitArray(string json, int start, int arity,
        System.Text.StringBuilder sb, out int next)
    {
        var values = new List<string>();
        if (!TrySplitValues(json, start + 1, arity, values) || values.Count != arity)
        {
            next = start;
            return false;
        }
        sb.Append('[');
        sb.Append(string.Join(",", values));
        sb.Append(']');
        next = json.IndexOf(']', start + 1) + 1;
        if (next == 0) { next = start; return false; }
        return true;
    }

    private static bool TrySplitValues(string json, int pos, int remaining, List<string> values)
    {
        if (remaining == 0) return pos >= json.Length || json[pos] == ']' || json[pos] == ',';
        foreach (var end in NumberCandidateEnds(json, pos))
        {
            values.Add(json.Substring(pos, end - pos));
            if (TrySplitValues(json, end, remaining - 1, values)) return true;
            values.RemoveAt(values.Count - 1);
        }
        return false;
    }

    /// <summary>End offsets of every valid JSON-number prefix at
    /// <paramref name="pos"/> (longest first), per the strict JSON grammar
    /// (no leading zeros, dot requires digits, optional exponent).</summary>
    private static IEnumerable<int> NumberCandidateEnds(string s, int pos)
    {
        var ends = new List<int>();
        int i = pos;
        if (i < s.Length && s[i] == '-') ++i;
        if (i >= s.Length || !char.IsDigit(s[i])) return ends;
        if (s[i] == '0')
        {
            // JSON forbids leading zeros: "0" is the whole integer part.
            ++i;
            ends.Add(i);
        }
        else
        {
            while (i < s.Length && char.IsDigit(s[i])) { ++i; ends.Add(i); }
        }
        if (i < s.Length && s[i] == '.')
        {
            int fracStart = ++i;
            while (i < s.Length && char.IsDigit(s[i])) { ++i; ends.Add(i); }
            if (i == fracStart) return ends; // lone dot: candidates = int part
        }
        if (i < s.Length && (s[i] == 'e' || s[i] == 'E'))
        {
            int j = i + 1;
            if (j < s.Length && (s[j] == '+' || s[j] == '-')) ++j;
            int expDigits = 0;
            while (j < s.Length && char.IsDigit(s[j])) { ++j; ++expDigits; ends.Add(j); }
        }
        ends.Sort();
        ends.Reverse();
        return ends;
    }

    private static bool JsonOnlyWhitespace(string s, int from, int to)
    {
        for (int i = from; i < to; ++i)
            if (!char.IsWhiteSpace(s[i])) return false;
        return true;
    }

    /// <summary>Engine emitter_config array lengths (ParticleTypes.h).</summary>
    private static readonly System.Collections.Generic.Dictionary<string, int> ArrayArities = new()
    {
        ["velocity_min"] = 3, ["velocity_max"] = 3, ["gravity"] = 3, ["wind"] = 3,
        ["color_start"] = 4, ["color_end"] = 4, ["scale_min"] = 2, ["scale_max"] = 2,
    };

    public void Write(string path)
    {
        File.WriteAllText(path, JsonSerializer.Serialize(this, Options));
    }

    /// <summary>Curve payload for a slot index (0..5, ParticleCurveSlot order), or null.</summary>
    public CurvePayload? GetCurve(int slot) => slot switch
    {
        0 => ScaleCurve,
        1 => AlphaCurve,
        2 => ColorGradientCurve,
        3 => VelocityCurve,
        4 => ForceCurve,
        5 => RotationCurve,
        _ => null,
    };

    public void SetCurve(int slot, CurvePayload? payload)
    {
        switch (slot)
        {
            case 0: ScaleCurve = payload; break;
            case 1: AlphaCurve = payload; break;
            case 2: ColorGradientCurve = payload; break;
            case 3: VelocityCurve = payload; break;
            case 4: ForceCurve = payload; break;
            case 5: RotationCurve = payload; break;
        }
    }

    /// <summary>Curve enable flag for a slot index (0..5).</summary>
    public bool IsCurveEnabled(int slot) => slot switch
    {
        0 => UseScaleCurve,
        1 => UseAlphaCurve,
        2 => UseColorGradient,
        3 => UseVelocityCurve,
        4 => UseForceCurve,
        5 => UseRotationCurve,
        _ => false,
    };

    public void SetCurveEnabled(int slot, bool enabled)
    {
        switch (slot)
        {
            case 0: UseScaleCurve = enabled; break;
            case 1: UseAlphaCurve = enabled; break;
            case 2: UseColorGradient = enabled; break;
            case 3: UseVelocityCurve = enabled; break;
            case 4: UseForceCurve = enabled; break;
            case 5: UseRotationCurve = enabled; break;
        }
    }
}
