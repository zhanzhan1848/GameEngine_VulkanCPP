using System;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Common;

/// <summary>
/// Regression coverage for the shared helper that pins and keeps the
/// managed pixel array alive for the lifetime of the host object
/// (typically a <c>WriteableBitmap</c>). The previous
/// <c>TextureTexrReader.BitmapFromSlice</c> and
/// <c>TextureEditorViewModel.RebuildPreview</c> released the
/// <see cref="System.Runtime.InteropServices.GCHandle"/> in
/// <c>finally</c>, leaving the byte[] free to be collected; the
/// Avalonia compositor then crashed in <c>_platform_memmove</c> when it
/// next read the now-freed pixel buffer (Content Browser preview of
/// compressed / normal-map textures, repeated clicks).
/// </summary>
public sealed class BitmapPinStoreTests
{
    [Fact]
    public void PinAndAttach_KeepsBytesReachableAfterCallerDropsReference()
    {
        var host = new object();
        byte[] pixelBuffer;
        IntPtr address;

        RecordKeepAlive(host, out pixelBuffer, out address);

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        var weak = new WeakReference<byte[]>(pixelBuffer);
        pixelBuffer = null!;

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        Assert.True(weak.TryGetTarget(out _));
        Assert.NotEqual(IntPtr.Zero, address);
    }

    [Fact]
    public void PinAndAttach_PinIsReleasedAfterHostIsGarbageCollected()
    {
        var weakHost = AttachToEphemeralHost();

        for (var i = 0; i < 4; ++i)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }

        Assert.False(weakHost.TryGetTarget(out _));
    }

    [Fact]
    public void PinAndAttach_ReturnsPinnedAddressAcrossReentries()
    {
        var host = new object();
        var first = BitmapPinStore.PinAndAttach(host, new byte[16]);
        var second = BitmapPinStore.PinAndAttach(host, new byte[32]);
        Assert.NotEqual(IntPtr.Zero, first);
        Assert.NotEqual(IntPtr.Zero, second);
    }

    private static void RecordKeepAlive(object host, out byte[] buffer, out IntPtr address)
    {
        buffer = new byte[8 * 8 * 4];
        for (var i = 0; i < buffer.Length; ++i) buffer[i] = (byte)i;
        address = BitmapPinStore.PinAndAttach(host, buffer);
    }

    private static WeakReference<object> AttachToEphemeralHost()
    {
        var host = new object();
        BitmapPinStore.PinAndAttach(host, new byte[4]);
        return new WeakReference<object>(host);
    }
}
