using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Pins a managed pixel buffer and ties its lifetime to a host object
/// (the <c>WriteableBitmap</c> produced from the buffer). The handle is
/// released automatically when the host is collected, eliminating the
/// use-after-free window caused by <c>GCHandle.Free</c> in
/// <c>finally</c> blocks of the previous implementation.
/// </summary>
internal static class BitmapPinStore
{
    private static readonly ConditionalWeakTable<object, Pin> _table = new();

    public static IntPtr PinAndAttach(object host, byte[] pixels)
    {
        ArgumentNullException.ThrowIfNull(host);
        ArgumentNullException.ThrowIfNull(pixels);

        if (_table.TryGetValue(host, out var existing))
        {
            existing.DisposeHandle();
            _table.Remove(host);
        }

        var handle = GCHandle.Alloc(pixels, GCHandleType.Pinned);
        var pin = new Pin(pixels, handle);
        _table.Add(host, pin);
        return handle.AddrOfPinnedObject();
    }

    private sealed class Pin
    {
        private readonly byte[] _pixels;
        private GCHandle _handle;

        internal Pin(byte[] pixels, GCHandle handle)
        {
            _pixels = pixels;
            _handle = handle;
        }

        internal void DisposeHandle()
        {
            if (_handle.IsAllocated)
            {
                _handle.Free();
                _handle = default;
            }
        }

        ~Pin()
        {
            DisposeHandle();
        }
    }
}
