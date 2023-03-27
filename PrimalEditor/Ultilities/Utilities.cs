using Microsoft.VisualBasic;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Threading;

namespace PrimalEditor.Ultilities
{
    public static class ID
    {
        //此处需要和Engine对齐INT位数
        public static int INVALID_ID => -1;// Engine内的int位数现在和c#内置是一致的，都是4位，如果Engine内设置的int位数不是4位，此处会引发错误
        public static bool IsValid(int id) => id != INVALID_ID;
    }

    public static class MathUtil
    {
        public static float Epsilon => 0.00001f;
        public static bool IsTheSameAs(this float value, float other)
        {
            return Math.Abs(value - other) < Epsilon;
        }
        public static bool IsTheSameAs(this float? value, float? other)
        {
            if(!value.HasValue || !other.HasValue) return false;
            return Math.Abs(value.Value - other.Value) < Epsilon;
        }

        public static long AlignSizeUp(long size, long alignment)
        {
            Debug.Assert(alignment > 0, "Alignment must be non-zero.");
            long mask = alignment- 1;
            Debug.Assert((alignment & mask) == 0, "Alignment should be a power of 2.");
            return ((size + mask) & ~mask);
        }

        public static long AlignSizeDown(long size, long alignment)
        {
            Debug.Assert(alignment > 0, "Alignment must be non-zero.");
            long mask = alignment - 1;
            Debug.Assert((alignment & mask) == 0, "Alignment should be a power of 2.");
            return (size & ~mask);
        }
    }

    class DelayEventTimerArgs : EventArgs
    {
        public bool RepeatEvent { get; set; }
        public IEnumerable<object> Data { get; set; }
        public DelayEventTimerArgs(IEnumerable<object> data)
        {
            Data = data;
        }
    }

    class DelayEventTimer
    {
        private readonly DispatcherTimer _timer;
        private readonly TimeSpan _delay;
        private readonly List<object> _data = new List<object>();
        private DateTime _lastEventTime = DateTime.Now;

        public event EventHandler<DelayEventTimerArgs> Triggered;

        public void Trigger(object data = null)
        {
            if(data != null)
            {
                _data.Add(data);
            }
            _lastEventTime = DateTime.Now;
            _timer.IsEnabled = true;
        }

        public void Disable()
        {
            _timer.IsEnabled = false;
        }

        private void OnTimerTick(object sender, EventArgs e)
        {
            if ((DateTime.Now - _lastEventTime) < _delay) return;
            var eventArgs = new DelayEventTimerArgs(_data);
            Triggered?.Invoke(this, eventArgs);
            if(!eventArgs.RepeatEvent)
            {
                _data.Clear();
            }
            _timer.IsEnabled = eventArgs.RepeatEvent;
        }

        public DelayEventTimer(TimeSpan delay, DispatcherPriority priority = DispatcherPriority.Normal)
        {
            _delay = delay;
            _timer = new DispatcherTimer(priority)
            {
                Interval = TimeSpan.FromMilliseconds(delay.TotalMilliseconds * 0.5)
            };
            _timer.Tick += OnTimerTick;
        }
    }
}
