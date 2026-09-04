using System;
using System.Collections.Generic;

namespace PrimalEditor_Avalonia.Editors.Particle;

// B4 P3: 曲线槽位与数据模型。wire 格式与 EngineDLL/ParticleEmitterAPI.cpp
// 的注释逐字节对齐（interpolation + keyCount + keys），改动必须双侧同步。

public enum ParticleCurveSlot
{
    Scale = 0,
    Alpha = 1,
    ColorGradient = 2,
    Velocity = 3,
    Force = 4,
    Rotation = 5,
}

public enum ParticleCurveInterpolation
{
    Constant = 0,
    Linear = 1,
    Smooth = 2,
    Bezier = 3,
}

/// <summary>
/// 统一键模型：Value 数组长度按槽位为 1（float）/ 4（RGBA）/ 3（XYZ）；
/// In/OutTangent 仅 float 槽位有效。
/// </summary>
public sealed class ParticleCurveKey
{
    public float Time { get; set; }
    public float[] Value { get; set; } = { 0f };
    public float InTangent { get; set; }
    public float OutTangent { get; set; }
}

public sealed class ParticleCurveData
{
    public ParticleCurveSlot Slot { get; set; }
    public ParticleCurveInterpolation Interpolation { get; set; } = ParticleCurveInterpolation.Linear;
    public List<ParticleCurveKey> Keys { get; set; } = new();

    public int ValueCount => Slot switch
    {
        ParticleCurveSlot.ColorGradient => 4,
        ParticleCurveSlot.Force => 3,
        _ => 1,
    };

    public bool HasTangents => ValueCount == 1;

    public static string SlotTitle(ParticleCurveSlot slot) => slot switch
    {
        ParticleCurveSlot.Scale => "Scale",
        ParticleCurveSlot.Alpha => "Alpha",
        ParticleCurveSlot.ColorGradient => "Color",
        ParticleCurveSlot.Velocity => "Velocity",
        ParticleCurveSlot.Force => "Force",
        ParticleCurveSlot.Rotation => "Rotation",
        _ => "?",
    };

    public static (float min, float max) SlotValueRange(ParticleCurveSlot slot) => slot switch
    {
        ParticleCurveSlot.Alpha => (0f, 1f),
        ParticleCurveSlot.Rotation => (-10f, 10f),
        ParticleCurveSlot.Force => (-20f, 20f),
        _ => (0f, 2f),
    };

    // ---- wire (de)serialization -----------------------------------------

    public static ParticleCurveData? FromWire(ParticleCurveSlot slot, byte[]? wire)
    {
        if (wire == null || wire.Length < 8) return null;
        var interp = BitConverter.ToUInt32(wire, 0);
        var count = BitConverter.ToUInt32(wire, 4);
        var data = new ParticleCurveData
        {
            Slot = slot,
            Interpolation = (ParticleCurveInterpolation)interp,
        };
        int stride = slot switch
        {
            ParticleCurveSlot.ColorGradient => 20,
            _ => 16,
        };
        if (wire.Length < 8 + (long)count * stride) return null;
        for (uint i = 0; i < count; ++i)
        {
            int off = 8 + (int)(i * stride);
            var key = new ParticleCurveKey { Time = BitConverter.ToSingle(wire, off) };
            var n = slot switch
            {
                ParticleCurveSlot.ColorGradient => 4,
                ParticleCurveSlot.Force => 3,
                _ => 1,
            };
            key.Value = new float[n];
            for (int v = 0; v < n; ++v)
                key.Value[v] = BitConverter.ToSingle(wire, off + 4 + v * 4);
            if (n == 1)
            {
                key.InTangent = BitConverter.ToSingle(wire, off + 8);
                key.OutTangent = BitConverter.ToSingle(wire, off + 12);
            }
            data.Keys.Add(key);
        }
        data.Keys.Sort((a, b) => a.Time.CompareTo(b.Time));
        return data;
    }

    public byte[] ToWire()
    {
        int stride = ValueCount == 1 ? 16 : ValueCount * 4 + 4;
        var wire = new byte[8 + Keys.Count * stride];
        BitConverter.GetBytes((uint)Interpolation).CopyTo(wire, 0);
        BitConverter.GetBytes((uint)Keys.Count).CopyTo(wire, 4);
        for (int i = 0; i < Keys.Count; ++i)
        {
            var k = Keys[i];
            int off = 8 + i * stride;
            BitConverter.GetBytes(k.Time).CopyTo(wire, off);
            for (int v = 0; v < ValueCount; ++v)
                BitConverter.GetBytes(k.Value[v]).CopyTo(wire, off + 4 + v * 4);
            if (ValueCount == 1)
            {
                BitConverter.GetBytes(k.InTangent).CopyTo(wire, off + 8);
                BitConverter.GetBytes(k.OutTangent).CopyTo(wire, off + 12);
            }
        }
        return wire;
    }
}
