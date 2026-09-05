using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>frame 时刻整条序列的求值快照：受轨驱动的实体变换 + 该帧取景相机。</summary>
public sealed class SequenceFrame
{
    /// <summary>有 Transform 轨覆盖的实体 → 该帧应取的变换（不含无轨实体）。</summary>
    public IReadOnlyDictionary<Guid, TransformData> Transforms { get; init; }
        = new Dictionary<Guid, TransformData>();

    /// <summary>CameraCut 命中的相机实体 guid；null = 无 cut 覆盖（回落编辑器相机）。</summary>
    public Guid? CameraEntityGuid { get; init; }
}

/// <summary>
/// 纯函数求值器（Part A 核心）。无状态、无随机、无墙钟——同一序列同一帧
/// 任意次求值结果逐位一致，这是 sequence.render 确定性 DoD 的根基。
/// </summary>
public static class SequenceEvaluator
{
    /// <summary>
    /// 单条 Transform 轨在 frame 处的值。帧区间外 clamp 到首/末关键帧值；
    /// 无关键帧返回 null。关键帧的 Interpolation 描述"本帧→右邻帧"的过渡：
    /// Constant 取本帧值保持到右邻帧；Linear 与右邻线性插值。
    /// </summary>
    public static TransformData? Evaluate(TransformTrack track, int frame)
    {
        var keys = track.Keys;
        if (keys.Count == 0) return null;

        // 定位区间：keys[lo].Frame <= frame < keys[lo+1].Frame（frame 在首帧前
        // 或末帧后时 lo 停在边界，等价 clamp）。
        int lo = 0;
        while (lo < keys.Count - 1 && keys[lo + 1].Frame <= frame) lo++;
        var left = keys[lo];
        if (lo == keys.Count - 1 || frame < keys[0].Frame ||
            left.Interpolation == KeyInterpolation.Constant || left.Frame == right().Frame)
            return ToData(left);
        var r = right();
        var t = (frame - left.Frame) / (float)(r.Frame - left.Frame);
        return Lerp(ToData(left), ToData(r), t);

        TransformKey right() => keys[lo + 1];

        static TransformData ToData(TransformKey k) => new()
        {
            PositionX = k.Position[0], PositionY = k.Position[1], PositionZ = k.Position[2],
            RotationX = k.RotationEuler[0], RotationY = k.RotationEuler[1], RotationZ = k.RotationEuler[2],
            ScaleX = k.Scale[0], ScaleY = k.Scale[1], ScaleZ = k.Scale[2],
        };

        static TransformData Lerp(TransformData a, TransformData b, float t) => new()
        {
            PositionX = a.PositionX + (b.PositionX - a.PositionX) * t,
            PositionY = a.PositionY + (b.PositionY - a.PositionY) * t,
            PositionZ = a.PositionZ + (b.PositionZ - a.PositionZ) * t,
            RotationX = a.RotationX + (b.RotationX - a.RotationX) * t,
            RotationY = a.RotationY + (b.RotationY - a.RotationY) * t,
            RotationZ = a.RotationZ + (b.RotationZ - a.RotationZ) * t,
            ScaleX = a.ScaleX + (b.ScaleX - a.ScaleX) * t,
            ScaleY = a.ScaleY + (b.ScaleY - a.ScaleY) * t,
            ScaleZ = a.ScaleZ + (b.ScaleZ - a.ScaleZ) * t,
        };
    }

    /// <summary>半开区间 [StartFrame, EndFrame) 查找 shot；越界/无效 shot 跳过。</summary>
    public static CameraShot? FindShot(CameraCutTrack track, int frame)
    {
        foreach (var shot in track.Shots)
            if (frame >= shot.StartFrame && frame < shot.EndFrame)
                return shot;
        return null;
    }

    /// <summary>frame 时刻整条序列快照。DurationFrames 之外的 frame 不 clamp
    /// （轨求值自行 clamp；cut 不命中即回落）——调用方负责帧区间合法性。</summary>
    public static SequenceFrame Evaluate(SequenceDocument doc, int frame)
    {
        var transforms = new Dictionary<Guid, TransformData>();
        Guid? camera = null;
        foreach (var track in doc.Tracks)
        {
            switch (track)
            {
                case TransformTrack tt:
                    var data = Evaluate(tt, frame);
                    if (data.HasValue) transforms[tt.EntityGuid] = data.Value;
                    break;
                case CameraCutTrack ct:
                    camera ??= FindShot(ct, frame)?.CameraEntityGuid;
                    break;
            }
        }
        return new SequenceFrame { Transforms = transforms, CameraEntityGuid = camera };
    }
}
