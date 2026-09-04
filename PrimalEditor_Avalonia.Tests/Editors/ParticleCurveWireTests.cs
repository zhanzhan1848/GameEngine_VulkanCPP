using System;
using PrimalEditor_Avalonia.Editors.Particle;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

// B4 P3: 曲线 wire 格式契约（与 EngineDLL/ParticleEmitterAPI.cpp 注释逐字节
// 对齐：u32 interpolation + u32 keyCount + keys）。序列化双侧在 C#，本测试
// 锁往返与键序；C++ 侧消费同一格式，改动 wire 必须双侧同步。
public sealed class ParticleCurveWireTests
{
    [Fact]
    public void FloatCurve_RoundTrips_InterpolationTangentsAndOrder()
    {
        var data = new ParticleCurveData
        {
            Slot = ParticleCurveSlot.Alpha,
            Interpolation = ParticleCurveInterpolation.Smooth,
        };
        data.Keys.Add(new ParticleCurveKey { Time = 1f, Value = new[] { 0f }, InTangent = -1f, OutTangent = 2f });
        data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { 1f }, InTangent = 0.5f, OutTangent = -0.5f });

        var restored = ParticleCurveData.FromWire(ParticleCurveSlot.Alpha, data.ToWire());

        Assert.NotNull(restored);
        Assert.Equal(ParticleCurveInterpolation.Smooth, restored!.Interpolation);
        Assert.Equal(2, restored.Keys.Count);
        // FromWire 按 time 排序——首键是 time=0 那个
        Assert.Equal(0f, restored.Keys[0].Time);
        Assert.Equal(1f, restored.Keys[0].Value[0]);
        Assert.Equal(0.5f, restored.Keys[0].InTangent);
        Assert.Equal(-0.5f, restored.Keys[0].OutTangent);
        Assert.Equal(1f, restored.Keys[1].Time);
        Assert.Equal(-1f, restored.Keys[1].InTangent);
        Assert.Equal(2f, restored.Keys[1].OutTangent);
    }

    [Fact]
    public void ColorGradient_RoundTrips_RgbaChannels()
    {
        var data = new ParticleCurveData { Slot = ParticleCurveSlot.ColorGradient };
        data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { 1f, 0.5f, 0.25f, 0f } });
        data.Keys.Add(new ParticleCurveKey { Time = 1f, Value = new[] { 0f, 1f, 0f, 1f } });

        var restored = ParticleCurveData.FromWire(ParticleCurveSlot.ColorGradient, data.ToWire());

        Assert.NotNull(restored);
        Assert.Equal(2, restored!.Keys.Count);
        Assert.Equal(new[] { 1f, 0.5f, 0.25f, 0f }, restored.Keys[0].Value);
        Assert.Equal(new[] { 0f, 1f, 0f, 1f }, restored.Keys[1].Value);
    }

    [Fact]
    public void VectorCurve_RoundTrips_Xyz()
    {
        var data = new ParticleCurveData
        {
            Slot = ParticleCurveSlot.Force,
            Interpolation = ParticleCurveInterpolation.Constant,
        };
        data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { 0f, -9.8f, 0f } });
        data.Keys.Add(new ParticleCurveKey { Time = 1f, Value = new[] { 3f, 0f, -1f } });

        var restored = ParticleCurveData.FromWire(ParticleCurveSlot.Force, data.ToWire());

        Assert.NotNull(restored);
        Assert.Equal(ParticleCurveInterpolation.Constant, restored!.Interpolation);
        Assert.Equal(new[] { 0f, -9.8f, 0f }, restored.Keys[0].Value);
        Assert.Equal(new[] { 3f, 0f, -1f }, restored.Keys[1].Value);
    }

    [Fact]
    public void FromWire_NullOrTruncated_ReturnsNull()
    {
        Assert.Null(ParticleCurveData.FromWire(ParticleCurveSlot.Alpha, null));
        Assert.Null(ParticleCurveData.FromWire(ParticleCurveSlot.Alpha, Array.Empty<byte>()));
        // 声明 4 键但数据只够 1 键 → 拒绝
        var truncated = new byte[8 + 16];
        BitConverter.GetBytes(1u).CopyTo(truncated, 0);
        BitConverter.GetBytes(4u).CopyTo(truncated, 4);
        Assert.Null(ParticleCurveData.FromWire(ParticleCurveSlot.Alpha, truncated));
    }

    [Fact]
    public void WireHeader_MatchesEngineContract()
    {
        // wire 头 8 字节 = interpolation + keyCount（小端 u32×2）
        var data = new ParticleCurveData { Slot = ParticleCurveSlot.Scale };
        data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { 1f } });
        var wire = data.ToWire();
        Assert.Equal(8 + 16, wire.Length); // 1 float 键 16B
        Assert.Equal(1u, BitConverter.ToUInt32(wire, 0)); // linear
        Assert.Equal(1u, BitConverter.ToUInt32(wire, 4)); // 1 key
    }
}
