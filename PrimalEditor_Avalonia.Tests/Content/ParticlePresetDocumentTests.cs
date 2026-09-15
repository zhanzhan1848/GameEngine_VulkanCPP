using System;
using System.IO;
using PrimalEditor_Avalonia.Content.Particles;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// .particle.json round-trip + engine-format compatibility. The engine's
/// hand-rolled from_json (Engine/Particles/ParticlePreset.cpp) finds values
/// by substring search for "key": — the serializer must stay compact so the
/// pattern survives, and every engine key must round-trip verbatim.
/// </summary>
public sealed class ParticlePresetDocumentTests
{
    private static ParticlePresetDocument Sample() => new()
    {
        Name = "fire_test",
        Category = "Fire",
        Description = "d",
        Author = "a",
        Version = 1,
        Config = new ParticlePresetDocument.EmitterConfig
        {
            MaxParticles = 2000,
            SpawnRate = 150f,
            BurstCount = 7,
            RingBufferMode = true,
            Mode = 0,
            LifetimeMin = 0.5f,
            LifetimeMax = 1.5f,
            VelocityMin = new[] { 0f, 2f, 0f },
            VelocityMax = new[] { 0f, 6f, 0f },
            ColorStart = new[] { 1f, 0.8f, 0.3f, 1f },
            ColorEnd = new[] { 1f, 0.2f, 0.05f, 0.6f },
            ScaleMin = new[] { 0.2f, 0.2f },
            ScaleMax = new[] { 0.9f, 0.9f },
            ScaleMultiplier = 1.5f,
            Gravity = new[] { 0f, -9.8f, 0f },
            Wind = new[] { 0.5f, 0f, 0f },
            Drag = 0.1f,
            BlendMode = 0,
            DepthWrite = false,
            TextureAtlasColumns = 4,
            TextureAtlasRows = 2,
            TextureRandomFrame = true,
            TextureFrameCount = 8,
        },
        UseScaleCurve = true,
        UseAlphaCurve = true,
        ScaleCurve = new ParticlePresetDocument.CurvePayload
        {
            Interpolation = 1,
            Keys =
            {
                new ParticlePresetDocument.CurveKeyPayload { Time = 0f, Value = new[] { 1.5f }, OutTangent = -1f },
                new ParticlePresetDocument.CurveKeyPayload { Time = 1f, Value = new[] { 0.2f } },
            },
        },
        AlphaCurve = new ParticlePresetDocument.CurvePayload
        {
            Interpolation = 2,
            Keys =
            {
                new ParticlePresetDocument.CurveKeyPayload { Time = 0f, Value = new[] { 1f, 1f, 1f, 1f } },
                new ParticlePresetDocument.CurveKeyPayload { Time = 1f, Value = new[] { 0f, 0f, 0f, 0f } },
            },
        },
    };

    [Fact]
    public void WriteRead_RoundTripsAllFields()
    {
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.particle.json");
        try
        {
            var doc = Sample();
            doc.Write(path);
            var read = ParticlePresetDocument.Read(path);

            Assert.Equal("fire_test", read.Name);
            Assert.Equal("Fire", read.Category);
            var c = read.Config;
            Assert.Equal(2000, c.MaxParticles);
            Assert.Equal(150f, c.SpawnRate);
            Assert.Equal(7, c.BurstCount);
            Assert.True(c.RingBufferMode);
            Assert.Equal(0.5f, c.LifetimeMin);
            Assert.Equal(1.5f, c.LifetimeMax);
            Assert.Equal(new[] { 0f, 2f, 0f }, c.VelocityMin);
            Assert.Equal(new[] { 1f, 0.8f, 0.3f, 1f }, c.ColorStart);
            Assert.Equal(new[] { 0.2f, 0.2f }, c.ScaleMin);
            Assert.Equal(1.5f, c.ScaleMultiplier);
            Assert.Equal(0.1f, c.Drag);
            Assert.Equal(4, c.TextureAtlasColumns);
            Assert.True(c.TextureRandomFrame);
            Assert.True(read.UseScaleCurve);
            Assert.False(read.UseForceCurve);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Write_ProducesEngineSearchableCompactJson()
    {
        var doc = Sample();
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.particle.json");
        try
        {
            doc.Write(path);
            var json = File.ReadAllText(path);

            // Engine's naive parser matches "key": with no space after the
            // colon; compact serialization must preserve that for scalars.
            foreach (var key in new[]
                     {
                         "name", "spawn_rate", "lifetime_min", "lifetime_max", "max_particles",
                         "velocity_min", "color_start", "color_end", "scale_min", "gravity",
                         "wind", "drag", "blend_mode", "depth_write", "use_scale_curve",
                     })
            {
                Assert.Contains($"\"{key}\":", json);
            }

            // Ring buffer must not shadow the engine's search for "mode":
            // "ring_buffer_mode": contains "_mode": but not "mode": — assert
            // the first "mode": occurrence is the standalone key.
            int standalone = json.IndexOf("\"mode\":", StringComparison.Ordinal);
            int ring = json.IndexOf("\"ring_buffer_mode\":", StringComparison.Ordinal);
            Assert.True(ring < 0 || standalone < ring || standalone > ring + "\"ring_buffer_mode\":".Length,
                "standalone mode key must not be confused by ring_buffer_mode");
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void CurveSlotAccessors_MapFlagsAndPayloads()
    {
        var doc = Sample();
        Assert.True(doc.IsCurveEnabled(0)); // scale
        Assert.True(doc.IsCurveEnabled(1)); // alpha
        Assert.False(doc.IsCurveEnabled(2)); // color gradient
        Assert.NotNull(doc.GetCurve(0));
        Assert.Equal(2, doc.GetCurve(0)!.Keys.Count);
        Assert.Equal(1, doc.GetCurve(0)!.Interpolation);
        Assert.Equal(4, doc.GetCurve(1)!.Keys[0].Value.Length);

        doc.SetCurveEnabled(4, true);
        Assert.True(doc.IsCurveEnabled(4));
        doc.SetCurve(4, null);
        Assert.Null(doc.GetCurve(4));
    }

    [Fact]
    public void Read_ToleratesEngineWrittenFileWithoutCurveKeys()
    {
        // Minimal file in exactly the shape preset_json::to_json emits
        // (no curve payload keys at all).
        var json = "{\"name\":\"fire\",\"category\":\"Fire\",\"description\":\"\",\"author\":\"\",\"version\":1," +
                   "\"config\":{\"max_particles\":1000,\"spawn_rate\":100,\"burst_count\":0," +
                   "\"ring_buffer_mode\":true,\"mode\":0,\"lifetime_min\":1,\"lifetime_max\":3," +
                   "\"velocity_min\":[0,1,0],\"velocity_max\":[0,5,0],\"color_start\":[1,1,1,1]," +
                   "\"color_end\":[0.5,0.5,1,0.8],\"scale_min\":[0.1,0.1],\"scale_max\":[1,1]," +
                   "\"scale_multiplier\":1,\"gravity\":[0,-9.8,0],\"wind\":[0,0,0],\"drag\":0," +
                   "\"blend_mode\":0,\"depth_write\":false,\"texture_atlas_columns\":1," +
                   "\"texture_atlas_rows\":1,\"texture_random_frame\":false,\"texture_frame_count\":1}," +
                   "\"use_scale_curve\":false,\"use_alpha_curve\":false,\"use_color_gradient\":false," +
                   "\"use_velocity_curve\":false,\"use_force_curve\":false,\"use_rotation_curve\":false}";
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.particle.json");
        try
        {
            File.WriteAllText(path, json);
            var read = ParticlePresetDocument.Read(path);
            Assert.Equal("fire", read.Name);
            Assert.Equal(100f, read.Config.SpawnRate);
            Assert.Null(read.GetCurve(0));
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Read_RepairsLegacyExportWithMissingArrayCommas()
    {
        // Verbatim bytes of a Dust.particle.json exported by the pre-fix
        // engine json_writer (begin_array reset the comma state, so element
        // separators were missing: [-0.2-0.1-0.2] instead of [-0.2,-0.1,-0.2]).
        const string broken = """{"name":"Dust","category":"Effects","description":"Floating dust particles","author":"","version":1,"config":{"max_particles":10000,"spawn_rate":10,"burst_count":0,"ring_buffer_mode":true,"mode":0,"lifetime_min":3,"lifetime_max":8,"velocity_min":[-0.2-0.1-0.2],"velocity_max":[0.20.10.2],"color_start":[0.80.70.60.5],"color_end":[0.60.50.40],"scale_min":[0.020.02],"scale_max":[0.10.1],"scale_multiplier":1,"gravity":[0-9.80],"wind":[000],"drag":1,"blend_mode":1,"depth_write":false,"texture_atlas_columns":1,"texture_atlas_rows":1,"texture_random_frame":false,"texture_frame_count":1},"use_scale_curve":false,"use_alpha_curve":false,"use_color_gradient":false,"use_velocity_curve":false,"use_force_curve":false,"use_rotation_curve":false}""";
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.particle.json");
        try
        {
            File.WriteAllText(path, broken);
            var read = ParticlePresetDocument.Read(path);

            Assert.Equal("Dust", read.Name);
            var c = read.Config;
            Assert.Equal(10f, c.SpawnRate);
            Assert.Equal(new[] { -0.2f, -0.1f, -0.2f }, c.VelocityMin);
            Assert.Equal(new[] { 0.2f, 0.1f, 0.2f }, c.VelocityMax);
            Assert.Equal(new[] { 0.8f, 0.7f, 0.6f, 0.5f }, c.ColorStart);
            Assert.Equal(new[] { 0.02f, 0.02f }, c.ScaleMin);
            Assert.Equal(new[] { 0f, -9.8f, 0f }, c.Gravity);
            Assert.Equal(new[] { 0f, 0f, 0f }, c.Wind);
            Assert.Equal(1, c.BlendMode);
        }
        finally
        {
            File.Delete(path);
        }
    }
}
