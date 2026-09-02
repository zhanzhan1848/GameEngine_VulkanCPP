using System;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 日志 L0（CLI 规划 §11.4）：通过 EngineDLL 的 InstallLogCallback 三件套把引擎
/// 事件流接到 C# 侧。Phase 0 只落实 stderr 纪律（引擎消息进 Console.Error，
/// 不进 UI 日志面板——面板接线是 L1，涉及跨线程 ObservableCollection）。
///
/// 线程契约（原生侧文档）：回调在引擎发出日志的线程上触发，任意线程都可能，
/// 因此这里只做 Console.Error.WriteLine（自身有锁），绝不触碰 Avalonia UI。
/// </summary>
internal static class EngineLogBridge
{
    /// <summary>与 EngineDLL/DiagnosticsAPI.h 的日志级别枚举严格一致。</summary>
    private const uint LevelDebug = 1;
    private const uint LevelInfo = 2;
    private const uint LevelWarn = 3;
    private const uint LevelError = 4;
    private const uint LevelFatal = 5;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LogCallbackDelegate(IntPtr userdata, uint level, string category, string message);

    [DllImport("PrimalEngine", EntryPoint = "InstallLogCallback", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InstallLogCallback(LogCallbackDelegate? callback, IntPtr userdata);

    [DllImport("PrimalEngine", EntryPoint = "SetLogLevel", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetLogLevel(uint level);

    private static LogCallbackDelegate? _callback;

    public static void Install()
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (_installedEvents || _callback != null) return;

        // 首选 §12 宿主事件表（on_log + on_device_lost，一张版本化表）；旧 dylib
        // 没有该导出时降级为 L0 的 InstallLogCallback 单回调。两条路只走一条，
        // 否则同一条日志会双份到达。
        var installError = EngineEventsBridge.Install(DeliverLog, DeliverDeviceLost);
        if (installError == null)
        {
            _installedEvents = true;
            SetLevelViaLegacy(LevelInfo);
            Logger.Info("[cli] 引擎事件表已安装 (info+, on_log/on_device_lost)");
            return;
        }

        _callback = OnLog;
        try
        {
            _InstallLogCallback(_callback, IntPtr.Zero);
            _SetLogLevel(LevelInfo); // info 及以上转达 C# 侧；trace/debug 留在原生环形缓冲
            Logger.Info("[cli] 引擎日志回调已安装 (info+)");
        }
        catch (Exception ex)
        {
            // 旧 dylib 没有这些导出时，降级为无桥接——不影响编辑器。
            _callback = null;
            Logger.Debug($"[cli] 引擎日志桥安装失败: {ex.Message} ({installError})");
        }
    }

    private static bool _installedEvents;

    // 事件表路径下没有 SetLogLevel 的封装入口——级别调节仍走 L0 导出。
    private static void SetLevelViaLegacy(uint level)
    {
        try { _SetLogLevel(level); } catch { }
    }

    public static void Uninstall()
    {
        if (_installedEvents)
        {
            EngineEventsBridge.Uninstall();
            _installedEvents = false;
            return;
        }
        if (_callback == null) return;
        try { _InstallLogCallback(null, IntPtr.Zero); } catch { }
        _callback = null;
    }

    private static void DeliverLog(uint level, string category, string message) => OnLog(IntPtr.Zero, level, category, message);

    private static void DeliverDeviceLost(string backend, string detail)
    {
        // 永不向原生侧抛异常：回调在 GPU 线程上自发触发（§12.3 线程契约）。
        try
        {
            Console.Error.WriteLine($"[{DateTime.Now:HH:mm:ss}] [ENGINE:DEVICE_LOST] [{backend}] {detail}");
            if (Avalonia.Application.Current != null)
            {
                var uiMessage = $"[ENGINE:{backend}] {detail}";
                Avalonia.Threading.Dispatcher.UIThread.Post(() =>
                {
                    try { s_uiLogger.Error(uiMessage); } catch { }
                });
            }
        }
        catch { }
    }

    private static void OnLog(IntPtr userdata, uint level, string category, string message)
    {
        // 永不向原生侧抛异常：回调在引擎任意线程上执行。
        try
        {
            var label = level switch
            {
                >= LevelError => "ERROR",
                LevelWarn => "WARN",
                LevelInfo => "INFO",
                _ => "DEBUG",
            };
            Console.Error.WriteLine($"[{DateTime.Now:HH:mm:ss}] [ENGINE:{label}] [{category}] {message}");

            // L1（CLI 规划 §11.4）：引擎消息进 UI 日志面板——三层首次打通。
            // ObservableCollection 非线程安全，Post 到 UI 线程后再入列；
            // fire-and-forget：日志路径绝不阻塞引擎线程。
            if (Avalonia.Application.Current != null)
            {
                var uiMessage = $"[ENGINE:{category}] {message}";
                Avalonia.Threading.Dispatcher.UIThread.Post(() =>
                {
                    try
                    {
                        if (level >= LevelError) s_uiLogger.Error(uiMessage);
                        else if (level >= LevelWarn) s_uiLogger.Warning(uiMessage);
                        else if (level >= LevelDebug) s_uiLogger.Info(uiMessage);
                        else s_uiLogger.Debug(uiMessage);
                    }
                    catch { /* UI 侧失败不影响引擎 */ }
                });
            }
        }
        catch
        {
            // swallow — diagnostics must not be able to kill the host
        }
    }

    private static readonly UiLogger s_uiLogger = new();
}
