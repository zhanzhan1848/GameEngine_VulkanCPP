using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>
/// 事件调度器（sequence S2）：播放推进时事件恰一次、seek 不误触。
///
/// 状态：_lastFrame（已触发边界——(last, current] 已触发）。
///   - Advance(toFrame)：前进播放一帧/多帧 → 触发 (last, to] 事件并更新 last；
///   - Seek(toFrame)：任意跳帧 → 只移动 last，**零触发**（scrub/编辑器预览语义）；
///   - Reset()/回绕（to < last）：按合同把回绕当重新开始——显式 Reset 后从 0；
///     未 Reset 直接回绕（如循环播放）→ 本调度器把 [to, last] 视作已消费，
///     从 to 继续正向（不补发——避免循环播放每圈重复触发累积）。
/// 派发经注入回调（宿主接 audio/door/任何业务；测试注入收集器）。
/// </summary>
public sealed class SequenceEventScheduler
{
    private readonly SequenceDocument _doc;
    private readonly Action<EventKey> _dispatch;
    private int _lastFrame;
    private bool _started;

    public int LastFrame => _lastFrame;

    public SequenceEventScheduler(SequenceDocument doc, Action<EventKey> dispatch)
    {
        _doc = doc;
        _dispatch = dispatch;
    }

    /// <summary>前进播放：触发 (last, toFrame] 的事件（恰一次语义的根基：
    /// 每帧的 Advance 只看增量区间，已过帧不重入）。</summary>
    public List<EventKey> Advance(int toFrame)
    {
        if (!_started)
        {
            // 首帧：起点本身的事件（frame == last）不触发——事件属于"经过"语义
            _started = true;
            _lastFrame = toFrame;
            return new List<EventKey>();
        }
        if (toFrame <= _lastFrame)
        {
            // 回绕：视为重新定位（不补发）——循环播放事件每圈触发需要显式 Reset
            _lastFrame = toFrame;
            return new List<EventKey>();
        }
        var events = SequenceEvaluator.EventsBetween(_doc, _lastFrame, toFrame);
        foreach (var e in events) _dispatch(e);
        _lastFrame = toFrame;
        return events;
    }

    /// <summary>跳帧（seek/scrub）：零触发，只移动边界。</summary>
    public void Seek(int toFrame)
    {
        _started = true;
        _lastFrame = toFrame;
    }

    /// <summary>重置（停止→再播放/循环新一圈）：边界回起点，后续 Advance 正常触发。</summary>
    public void Reset(int frame = 0)
    {
        _started = false;
        _lastFrame = frame;
    }
}
