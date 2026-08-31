using System;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// Per-frame tick that drives async asset uploads and keeps the C++ engine in sync.
/// Hooked into Avalonia's render loop.
/// </summary>
public static class EngineTick
{
    private static DispatcherTimer? _timer;
    private static int _lastPendingCount;

    public static void Start()
    {
        if (_timer != null) return;
        _timer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(16),
        };
        _timer.Tick += OnTick;
        _timer.Start();
        Logger.Info("Engine tick started (16ms interval)");
    }

    public static void Stop()
    {
        _timer?.Stop();
        _timer = null;
        Logger.Info("Engine tick stopped");
    }

    private static void OnTick(object? sender, EventArgs e)
    {
        if (!NativeEngineBridge.IsLoaded) return;

        try
        {
            NativeEngineBridge.TickAssetSystem();

            var pending = (int)NativeEngineBridge.GetPendingUploadCount();
            if (pending != _lastPendingCount)
            {
                _lastPendingCount = pending;
                if (pending > 0)
                {
                    Logger.Debug($"Pending asset uploads: {pending}");
                }
            }
        }
        catch (Exception ex)
        {
            Logger.Error("Engine tick failed", ex);
        }
    }
}