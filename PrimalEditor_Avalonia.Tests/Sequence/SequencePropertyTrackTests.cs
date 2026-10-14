using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Sequence;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.SequenceNs;

/// <summary>
/// sequence S1 属性轨契约测试（sequence-S1-A01 数据侧）：
/// 帧0值0/帧60值10——linear 帧30=5、constant 帧30=0；区间外 clamp；
/// 空轨/不支持属性 null；序列化往返（多态轨）；运行包行构建（含跳过语义）。
/// </summary>
public sealed class SequencePropertyTrackTests
{
    private static PropertyTrack MakeLinearTrack() => new()
    {
        EntityGuid = Guid.NewGuid(),
        Keys =
        {
            new PropertyKey { Frame = 0, Value = 0f, Interpolation = KeyInterpolation.Linear },
            new PropertyKey { Frame = 60, Value = 10f },
        },
    };

    [Fact]
    public void A01_Linear_MidFrame_IsHalf()
    {
        var track = MakeLinearTrack();
        Assert.Equal(0f, SequenceEvaluator.EvaluateProperty(track, 0));
        Assert.Equal(5f, SequenceEvaluator.EvaluateProperty(track, 30));
        Assert.Equal(10f, SequenceEvaluator.EvaluateProperty(track, 60));
    }

    [Fact]
    public void A01_Constant_MidFrame_HoldsLeftValue()
    {
        var track = MakeLinearTrack();
        track.Keys[0].Interpolation = KeyInterpolation.Constant;
        Assert.Equal(0f, SequenceEvaluator.EvaluateProperty(track, 30));
        Assert.Equal(0f, SequenceEvaluator.EvaluateProperty(track, 59));
        Assert.Equal(10f, SequenceEvaluator.EvaluateProperty(track, 60));   // 末关键帧 clamp
    }

    [Fact]
    public void Evaluation_Clamps_Outside_Key_Range()
    {
        var track = MakeLinearTrack();
        Assert.Equal(0f, SequenceEvaluator.EvaluateProperty(track, -10));   // 首帧前 → 首帧值
        Assert.Equal(10f, SequenceEvaluator.EvaluateProperty(track, 1000)); // 末帧后 → 末帧值
    }

    [Fact]
    public void Evaluation_EmptyTrack_And_UnsupportedProperty_AreNull()
    {
        Assert.Null(SequenceEvaluator.EvaluateProperty(new PropertyTrack(), 0));   // 空轨
        var bad = MakeLinearTrack();
        bad.Component = "hair";
        bad.Property = "style";
        Assert.Null(SequenceEvaluator.EvaluateProperty(bad, 30));                  // 白名单外
        Assert.False(PropertyTrack.IsSupported("hair", "style"));
        Assert.True(PropertyTrack.IsSupported("light", "intensity"));
    }

    [Fact]
    public void DocumentEvaluate_Includes_Property_Samples()
    {
        var guid = Guid.NewGuid();
        var doc = new SequenceDocument();
        doc.Tracks.Add(new PropertyTrack
        {
            EntityGuid = guid,
            Keys =
            {
                new PropertyKey { Frame = 0, Value = 1f },
                new PropertyKey { Frame = 60, Value = 8f },
            },
        });
        var snapshot = SequenceEvaluator.Evaluate(doc, 30);
        var sample = Assert.Single(snapshot.Properties);
        Assert.Equal(guid, sample.EntityGuid);
        Assert.Equal("light", sample.Component);
        Assert.Equal("intensity", sample.Property);
        Assert.Equal(4.5f, sample.Value, 5);
    }

    [Fact]
    public void Serialization_RoundTrip_Preserves_PropertyTrack()
    {
        var doc = new SequenceDocument { Name = "S1", Fps = 30, DurationFrames = 61 };
        doc.Tracks.Add(new TransformTrack
        {
            EntityGuid = Guid.NewGuid(),
            Keys = { new TransformKey { Frame = 0, Position = new float[] { 1, 2, 3 } } },
        });
        doc.Tracks.Add(new PropertyTrack
        {
            EntityGuid = Guid.NewGuid(),
            Component = "light",
            Property = "intensity",
            Keys =
            {
                new PropertyKey { Frame = 0, Value = 0f },
                new PropertyKey { Frame = 30, Value = 5f, Interpolation = KeyInterpolation.Constant },
                new PropertyKey { Frame = 60, Value = 10f },
            },
        });

        var path = Path.Combine(Path.GetTempPath(), $"s1_{Guid.NewGuid():N}.pseq");
        doc.Save(path);
        var loaded = SequenceDocument.Load(path);
        File.Delete(path);

        var pt = Assert.Single(loaded.Tracks.OfType<PropertyTrack>());
        Assert.Equal("light", pt.Component);
        Assert.Equal("intensity", pt.Property);
        Assert.Equal(3, pt.Keys.Count);
        Assert.Equal(30, pt.Keys[1].Frame);
        Assert.Equal(5f, pt.Keys[1].Value);
        Assert.Equal(KeyInterpolation.Constant, pt.Keys[1].Interpolation);
        // 求值一致性：重载文档与原文档逐位同值（A01"保存重开一致"）
        Assert.Equal(5f, SequenceEvaluator.EvaluateProperty(pt, 30));
        Assert.Equal(10f, SequenceEvaluator.EvaluateProperty(pt, 60));
    }

    [Fact]
    public void Rows_Build_EmitsSeqRows_And_SkipsMissingTargets()
    {
        var e1 = Guid.NewGuid();
        var e2 = Guid.NewGuid();
        var doc = new SequenceDocument { Fps = 60, DurationFrames = 61 };
        doc.Tracks.Add(new PropertyTrack
        {
            EntityGuid = e1,
            Keys = { new PropertyKey { Frame = 0, Value = 0f }, new PropertyKey { Frame = 60, Value = 10f } },
        });
        doc.Tracks.Add(new TransformTrack
        {
            EntityGuid = e2,
            Keys = { new TransformKey { Frame = 0, Position = new float[] { 0, 4, 0 }, Scale = new float[] { 1, 1, 1 } } },
        });

        var (lines, skipped) = SequenceRows.Build(doc, guid =>
            guid == e1 ? "sun" : guid == e2 ? null : "?");
        Assert.Equal(
            new[]
            {
                "seqmeta 60 61",
                "seqprop \"sun\" light intensity 2",
                "seqkey 0 0 0",
                "seqkey 60 10 0",
            },
            lines);
        Assert.Single(skipped);   // e2 无名（目标不在场景）→ transform 轨跳过
    }
}
