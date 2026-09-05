using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Sequence;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Sequence;

/// <summary>
/// Sequence 数据模型与求值器（Part A，无 GPU 依赖）。求值确定性是
/// sequence.render 逐帧一致 DoD 的根基；序列化 roundtrip 保证 CLI/UI 两端
/// 读写同一份 .pseq。
/// </summary>
public sealed class SequenceEvaluationTests
{
    private static TransformKey Key(int frame, float x, KeyInterpolation interp = KeyInterpolation.Linear) =>
        new()
        {
            Frame = frame,
            Position = new float[] { x, 0, 0 },
            RotationEuler = new float[] { 0, 0, 0 },
            Scale = new float[] { 1, 1, 1 },
            Interpolation = interp,
        };

    private static TransformTrack Track(params TransformKey[] keys)
    {
        var track = new TransformTrack { EntityGuid = Guid.NewGuid(), Name = "T" };
        track.Keys.AddRange(keys);
        return track;
    }

    // ------------------------------------------------------------------
    // 变换轨求值：线性 / 阶梯 / clamp / 空轨
    // ------------------------------------------------------------------

    [Fact]
    public void Evaluate_Linear_InterpolatesBetweenKeys()
    {
        var track = Track(Key(0, 0), Key(10, 10));
        var mid = SequenceEvaluator.Evaluate(track, 5)!.Value;
        Assert.Equal(5f, mid.PositionX, 5);

        var quarter = SequenceEvaluator.Evaluate(track, 2)!.Value;
        Assert.Equal(2f, quarter.PositionX, 5);
    }

    [Fact]
    public void Evaluate_Constant_HoldsLeftKeyUntilNextKey()
    {
        var track = Track(Key(0, 1, KeyInterpolation.Constant), Key(10, 9));
        Assert.Equal(1f, SequenceEvaluator.Evaluate(track, 4)!.Value.PositionX, 5);
        Assert.Equal(1f, SequenceEvaluator.Evaluate(track, 9)!.Value.PositionX, 5);
        // 到达右邻关键帧即切换为右邻值
        Assert.Equal(9f, SequenceEvaluator.Evaluate(track, 10)!.Value.PositionX, 5);
    }

    [Fact]
    public void Evaluate_ClampsOutsideKeyRange()
    {
        var track = Track(Key(10, 3), Key(20, 7));
        Assert.Equal(3f, SequenceEvaluator.Evaluate(track, 0)!.Value.PositionX, 5);
        Assert.Equal(3f, SequenceEvaluator.Evaluate(track, 5)!.Value.PositionX, 5);
        Assert.Equal(7f, SequenceEvaluator.Evaluate(track, 20)!.Value.PositionX, 5);
        Assert.Equal(7f, SequenceEvaluator.Evaluate(track, 999)!.Value.PositionX, 5);
    }

    [Fact]
    public void Evaluate_EmptyTrack_ReturnsNull()
    {
        Assert.Null(SequenceEvaluator.Evaluate(Track(), 0));
    }

    [Fact]
    public void Evaluate_DuplicateFrameKeys_DoesNotDivideByZero()
    {
        // 同帧双 key：区间定位会跳过与左端同帧的 key，不产生 0/0 除法。
        var track = Track(Key(5, 1), Key(5, 2), Key(10, 3));
        var value = SequenceEvaluator.Evaluate(track, 7)!.Value;
        Assert.Equal(2.4f, value.PositionX, 5);
    }

    // ------------------------------------------------------------------
    // 相机 cut：半开区间 / 空隙回落 / 无效 shot
    // ------------------------------------------------------------------

    private static CameraCutTrack Cut(params CameraShot[] shots)
    {
        var track = new CameraCutTrack { Name = "Cuts" };
        track.Shots.AddRange(shots);
        return track;
    }

    private static CameraShot Shot(int start, int end, Guid cam) =>
        new() { StartFrame = start, EndFrame = end, CameraEntityGuid = cam };

    [Fact]
    public void FindShot_HalfOpenInterval()
    {
        var camA = Guid.NewGuid();
        var camB = Guid.NewGuid();
        var track = Cut(Shot(0, 10, camA), Shot(10, 20, camB));

        Assert.Equal(camA, SequenceEvaluator.FindShot(track, 0)!.CameraEntityGuid);
        Assert.Equal(camA, SequenceEvaluator.FindShot(track, 9)!.CameraEntityGuid);
        // 半开区间：第 10 帧属于第二个 shot（相邻 shot 边界不重叠）
        Assert.Equal(camB, SequenceEvaluator.FindShot(track, 10)!.CameraEntityGuid);
        Assert.Null(SequenceEvaluator.FindShot(track, 20));
    }

    [Fact]
    public void FindShot_GapBetweenShots_FallsBackToNull()
    {
        var track = Cut(Shot(0, 5, Guid.NewGuid()), Shot(10, 20, Guid.NewGuid()));
        Assert.Null(SequenceEvaluator.FindShot(track, 7));
    }

    [Fact]
    public void FindShot_InvalidShot_EndBeforeStart_IsSkipped()
    {
        var cam = Guid.NewGuid();
        var track = Cut(Shot(15, 10, cam), Shot(20, 30, cam));
        Assert.Null(SequenceEvaluator.FindShot(track, 12));
        Assert.Equal(cam, SequenceEvaluator.FindShot(track, 25)!.CameraEntityGuid);
    }

    // ------------------------------------------------------------------
    // 整条序列快照
    // ------------------------------------------------------------------

    [Fact]
    public void Evaluate_Sequence_CollectsTracksAndCamera()
    {
        var doc = new SequenceDocument { Fps = 30, DurationFrames = 100 };
        var entityA = Guid.NewGuid();
        var camA = Guid.NewGuid();

        doc.Tracks.Add(new TransformTrack { EntityGuid = entityA, Keys = { Key(0, 0), Key(10, 10) } });
        doc.Tracks.Add(Cut(Shot(0, 50, camA)));

        var frame = SequenceEvaluator.Evaluate(doc, 5);
        Assert.True(frame.Transforms.ContainsKey(entityA));
        Assert.Equal(5f, frame.Transforms[entityA].PositionX, 5);
        Assert.Equal(camA, frame.CameraEntityGuid);

        var afterCut = SequenceEvaluator.Evaluate(doc, 60);
        Assert.Null(afterCut.CameraEntityGuid); // cut 空隙回落编辑器相机
    }

    // ------------------------------------------------------------------
    // 序列化 roundtrip（CLI/UI 读写同一份 .pseq 的契约）
    // ------------------------------------------------------------------

    [Fact]
    public void SaveLoad_Roundtrip_PreservesTracksAndKeys()
    {
        var path = Path.Combine(Path.GetTempPath(), $"pseq_test_{Guid.NewGuid():N}.pseq");
        try
        {
            var doc = new SequenceDocument { Name = "Intro", Fps = 24, DurationFrames = 48 };
            var entity = Guid.NewGuid();
            var cam = Guid.NewGuid();
            var track = new TransformTrack
            {
                EntityGuid = entity,
                Keys =
                {
                    new TransformKey
                    {
                        Frame = 0,
                        Position = new[] { 1f, 2f, 3f },
                        RotationEuler = new[] { 0f, 90f, 0f },
                        Scale = new[] { 2f, 2f, 2f },
                        Interpolation = KeyInterpolation.Constant,
                    },
                    Key(24, 5),
                },
            };
            doc.Tracks.Add(track);
            doc.Tracks.Add(Cut(Shot(0, 24, cam), Shot(24, 48, cam)));

            doc.Save(path);
            var loaded = SequenceDocument.Load(path);

            Assert.Equal(24, loaded.Fps);
            Assert.Equal(48, loaded.DurationFrames);
            var loadedTrack = Assert.IsType<TransformTrack>(loaded.Tracks[0]);
            Assert.Equal(entity, loadedTrack.EntityGuid);
            Assert.Equal(2, loadedTrack.Keys.Count);
            Assert.Equal(KeyInterpolation.Constant, loadedTrack.Keys[0].Interpolation);
            Assert.Equal(90f, loadedTrack.Keys[0].RotationEuler[1]);
            var loadedCut = Assert.IsType<CameraCutTrack>(loaded.Tracks[1]);
            Assert.Equal(cam, loadedCut.Shots[1].CameraEntityGuid);

            // roundtrip 后求值一致
            Assert.Equal(
                SequenceEvaluator.Evaluate(doc, 12).Transforms.Values.First().PositionX,
                SequenceEvaluator.Evaluate(loaded, 12).Transforms.Values.First().PositionX);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Load_VersionTooNew_Throws()
    {
        var path = Path.Combine(Path.GetTempPath(), $"pseq_test_{Guid.NewGuid():N}.pseq");
        try
        {
            File.WriteAllText(path, """{"version":999,"name":"x","fps":30,"durationFrames":1,"tracks":[]}""");
            Assert.Throws<InvalidOperationException>(() => SequenceDocument.Load(path));
        }
        finally
        {
            File.Delete(path);
        }
    }

    // ------------------------------------------------------------------
    // Undo 动作：键帧增删/同帧替换的精确往返
    // ------------------------------------------------------------------

    [Fact]
    public void AddKeyAction_SameFrameReplace_UndoRestoresOriginal()
    {
        var track = Track(Key(0, 1), Key(10, 2));
        var world = new PrimalEditor_Avalonia.Common.UndoRedo();
        var action = new SequenceUndoActions.AddKeyAction(track, Key(0, 99));
        world.Add(action);
        action.Redo();

        Assert.Single(track.Keys.Where(k => k.Frame == 0));
        Assert.Equal(99f, track.Keys[0].Position[0]);

        world.Undo();
        Assert.Equal(1f, track.Keys[0].Position[0]);

        world.Redo();
        Assert.Equal(99f, track.Keys[0].Position[0]);
    }

    [Fact]
    public void RemoveTrackAction_UndoRestoresAtOriginalIndex()
    {
        var doc = new SequenceDocument();
        doc.Tracks.Add(Track(Key(0, 1)));
        var cut = Cut(Shot(0, 10, Guid.NewGuid()));
        doc.Tracks.Add(cut);
        doc.Tracks.Add(Track(Key(0, 2)));

        var action = new SequenceUndoActions.RemoveTrackAction(doc, cut);
        action.Redo();
        Assert.DoesNotContain(cut, doc.Tracks);
        action.Undo();
        Assert.Same(cut, doc.Tracks[1]);
    }
}
