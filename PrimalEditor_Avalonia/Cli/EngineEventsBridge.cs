using System;
using System.Runtime.InteropServices;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 宿主事件表（CLI 规划 §12）的 C# 镜像：与 EngineDLL/EngineEventsAPI.h 的
/// primal_engine_events_v1 布局逐字段对齐（Sequential、64 字节，安装时断言），
/// 编辑器作为平级消费者接收引擎事件流（on_log / on_device_lost；assert/crash/
/// progress 为表内占位，v1 不触发）。
/// </summary>
internal static class EngineEventsBridge
{
    private const uint PrimalEventsV1 = 1;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void OnLogDelegate(IntPtr userdata, uint level, string category,
        string message, ref LogFields fields);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void OnAssertDelegate(IntPtr userdata, string file, int line, string condition);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void OnCrashDelegate(IntPtr userdata, ref CrashInfo info);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void OnProgressDelegate(IntPtr userdata, string operation, int done, int total);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void OnDeviceLostDelegate(IntPtr userdata, string backend, string detail);

    [StructLayout(LayoutKind.Sequential)]
    internal struct LogFields
    {
        public ulong Frame;
        public long ThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CrashInfo
    {
        public IntPtr Reason;
        public IntPtr Blackbox;
        public uint BlackboxLen;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct EngineEventsV1
    {
        public uint Version;
        public UIntPtr Size;          // size_t
        public IntPtr Userdata;
        public OnLogDelegate OnLog;
        public OnAssertDelegate OnAssert;
        public OnCrashDelegate OnCrash;
        public OnProgressDelegate OnProgress;
        public OnDeviceLostDelegate OnDeviceLost;
    }

    [DllImport("PrimalEngine", EntryPoint = "InstallEngineEvents", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InstallEngineEvents(ref EngineEventsV1 events);

    [DllImport("PrimalEngine", EntryPoint = "InstallEngineEvents", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InstallEngineEvents(IntPtr events);

    // 委托由静态字段持有：原生侧保存函数指针，GC 回收 = 回调跳进已释放 thunk。
    private static OnLogDelegate? _onLog;
    private static OnAssertDelegate? _onAssert;
    private static OnCrashDelegate? _onCrash;
    private static OnProgressDelegate? _onProgress;
    private static OnDeviceLostDelegate? _onDeviceLost;

    /// <summary>安装事件表。返回 null=成功；其他字符串为失败原因（调用方降级）。</summary>
    internal static string? Install(Action<uint, string, string> onLog, Action<string, string>? onDeviceLost = null)
    {
        try
        {
            var expectedSize = Marshal.SizeOf(typeof(EngineEventsV1));
            if (expectedSize != 64)
                return $"events vtable size drift: {expectedSize} != 64";

            // 先落到静态字段再装表：安装瞬间原生侧就可能回调。
            _onLog = (IntPtr ud, uint level, string category, string message, ref LogFields fields) =>
                onLog(level, category, message);
            _onAssert = null;   // v1 占位不触发
            _onCrash = null;
            _onProgress = null;
            _onDeviceLost = onDeviceLost == null ? null : (ud, backend, detail) => onDeviceLost(backend, detail);

            var table = new EngineEventsV1
            {
                Version = PrimalEventsV1,
                Size = (UIntPtr)(ulong)expectedSize,
                Userdata = IntPtr.Zero,
                OnLog = _onLog!,
                OnAssert = _onAssert!,
                OnCrash = _onCrash!,
                OnProgress = _onProgress!,
                OnDeviceLost = _onDeviceLost!,
            };
            return _InstallEngineEvents(ref table) == 1 ? null : "engine rejected events vtable (version/size)";
        }
        catch (MissingMethodException)
        {
            return "InstallEngineEvents export missing (older dylib)";
        }
        catch (EntryPointNotFoundException)
        {
            return "InstallEngineEvents export missing (older dylib)";
        }
        catch (Exception ex)
        {
            return ex.Message;
        }
    }

    internal static void Uninstall()
    {
        try { _InstallEngineEvents(IntPtr.Zero); } catch { }
        _onLog = null;
        _onAssert = null;
        _onCrash = null;
        _onProgress = null;
        _onDeviceLost = null;
    }
}
