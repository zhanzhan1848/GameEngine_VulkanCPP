using System;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// P/Invoke bindings for <c>EngineDLL/MaterialPreviewAPI.cpp</c> — owns a
/// preview surface that renders a sphere/plane mesh using the supplied
/// material graph JSON. The C# side feeds it the latest serialized graph
/// after every edit, plus the live RHI device/command-buffer handles.
/// </summary>
public static class MaterialPreviewNative
{
    private const string Lib = "PrimalEngine";

    [DllImport(Lib, EntryPoint = "CreateMaterialPreview", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateMaterialPreview(ulong deviceHandle, uint width, uint height);

    [DllImport(Lib, EntryPoint = "DestroyMaterialPreview", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyMaterialPreview(uint previewId);

    [DllImport(Lib, EntryPoint = "DestroyAllMaterialPreviews", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyAllMaterialPreviews();

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewGraph", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetMaterialPreviewGraph(uint previewId, IntPtr graphJson);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewModel", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewModel(uint previewId, uint modelType);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewMesh", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetMaterialPreviewMesh(uint previewId, ulong geometryContentId);

    [DllImport(Lib, EntryPoint = "ClearMaterialPreviewMesh", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ClearMaterialPreviewMesh(uint previewId);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewMode2D", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewMode2D(uint previewId, uint enable);

    [DllImport(Lib, EntryPoint = "IsMaterialPreviewMode2D", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _IsMaterialPreviewMode2D(uint previewId);

    [DllImport(Lib, EntryPoint = "ResizeMaterialPreview", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ResizeMaterialPreview(uint previewId, uint width, uint height);

    [DllImport(Lib, EntryPoint = "RenderMaterialPreview", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong _RenderMaterialPreview(uint previewId, ulong cmdHandle, uint frameIndex, float dt);

    [DllImport(Lib, EntryPoint = "BlitMaterialPreviewToBackbuffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _BlitMaterialPreviewToBackbuffer(uint previewId, ulong cmdHandle, ulong dstTextureHandle, uint dstWidth, uint dstHeight);

    [DllImport(Lib, EntryPoint = "GetMaterialPreviewTexture", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong _GetMaterialPreviewTexture(uint previewId);

    [DllImport(Lib, EntryPoint = "GetMaterialPreviewWidth", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetMaterialPreviewWidth(uint previewId);

    [DllImport(Lib, EntryPoint = "GetMaterialPreviewHeight", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetMaterialPreviewHeight(uint previewId);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewCameraPosition", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewCameraPosition(uint previewId, IntPtr xyz);

    [DllImport(Lib, EntryPoint = "GetMaterialPreviewCameraPosition", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GetMaterialPreviewCameraPosition(uint previewId, IntPtr outXyz);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewCameraTarget", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewCameraTarget(uint previewId, IntPtr xyz);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewCameraFov", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewCameraFov(uint previewId, float radians);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewRotationSpeed", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewRotationSpeed(uint previewId, float radPerSec);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewLightDirection", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewLightDirection(uint previewId, IntPtr normalizedDir);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewLightColor", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewLightColor(uint previewId, IntPtr rgb);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewLightIntensity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewLightIntensity(uint previewId, float intensity);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewBackgroundMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewBackgroundMode(uint previewId, uint mode);

    [DllImport(Lib, EntryPoint = "SetMaterialPreviewBackgroundColors", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialPreviewBackgroundColors(uint previewId, IntPtr topRgb, IntPtr botRgb);

    // One-shot sync render. Caller owns the output buffer; native side handles
    // cmd buffer + readback internally. Returns bytes written (= width*height*4).
    [DllImport(Lib, EntryPoint = "RenderMaterialPreviewToRGBA", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong _RenderMaterialPreviewToRGBA(uint previewId, ulong deviceHandle, IntPtr outRgba, ulong bufferSize);

    // Lifecycle

    public static uint Create(ulong deviceHandle, uint width, uint height) =>
        Safe(() => _CreateMaterialPreview(deviceHandle, width, height));

    public static void Destroy(uint previewId) => Safe(() => _DestroyMaterialPreview(previewId));

    public static void DestroyAll() => Safe(() => _DestroyAllMaterialPreviews());

    public static bool SetGraph(uint previewId, string graphJson)
    {
        if (string.IsNullOrEmpty(graphJson)) return false;
        var bytes = System.Text.Encoding.UTF8.GetBytes(graphJson + '\0');
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try { return Safe(() => _SetMaterialPreviewGraph(previewId, pin.AddrOfPinnedObject())) != 0; }
        finally { pin.Free(); }
    }

    public static void SetModel(uint previewId, uint modelType) =>
        Safe(() => _SetMaterialPreviewModel(previewId, modelType));

    public static bool SetMesh(uint previewId, ulong geometryContentId) =>
        Safe(() => _SetMaterialPreviewMesh(previewId, geometryContentId)) != 0;

    public static void ClearMesh(uint previewId) => Safe(() => _ClearMaterialPreviewMesh(previewId));

    public static void SetMode2D(uint previewId, bool enable) => Safe(() => _SetMaterialPreviewMode2D(previewId, enable ? 1u : 0u));

    public static bool IsMode2D(uint previewId) => Safe(() => _IsMaterialPreviewMode2D(previewId)) != 0;

    public static void Resize(uint previewId, uint width, uint height) =>
        Safe(() => _ResizeMaterialPreview(previewId, width, height));

    public static ulong Render(uint previewId, ulong cmdHandle, uint frameIndex, float dt) =>
        Safe(() => _RenderMaterialPreview(previewId, cmdHandle, frameIndex, dt));

    public static void BlitToBackbuffer(uint previewId, ulong cmdHandle, ulong dstTexture, uint dstWidth, uint dstHeight) =>
        Safe(() => _BlitMaterialPreviewToBackbuffer(previewId, cmdHandle, dstTexture, dstWidth, dstHeight));

    public static ulong GetOutputTexture(uint previewId) => Safe(() => _GetMaterialPreviewTexture(previewId));

    public static uint GetWidth(uint previewId) => Safe(() => _GetMaterialPreviewWidth(previewId));

    public static uint GetHeight(uint previewId) => Safe(() => _GetMaterialPreviewHeight(previewId));

    public static void SetCameraPosition(uint previewId, float x, float y, float z)
    {
        var v = new[] { x, y, z };
        var pin = GCHandle.Alloc(v, GCHandleType.Pinned);
        try { Safe(() => _SetMaterialPreviewCameraPosition(previewId, pin.AddrOfPinnedObject())); }
        finally { pin.Free(); }
    }

    public static float[] GetCameraPosition(uint previewId)
    {
        var v = new float[3];
        var pin = GCHandle.Alloc(v, GCHandleType.Pinned);
        try { Safe(() => _GetMaterialPreviewCameraPosition(previewId, pin.AddrOfPinnedObject())); }
        finally { pin.Free(); }
        return v;
    }

    public static void SetCameraTarget(uint previewId, float x, float y, float z)
    {
        var v = new[] { x, y, z };
        var pin = GCHandle.Alloc(v, GCHandleType.Pinned);
        try { Safe(() => _SetMaterialPreviewCameraTarget(previewId, pin.AddrOfPinnedObject())); }
        finally { pin.Free(); }
    }

    public static void SetCameraFov(uint previewId, float radians) =>
        Safe(() => _SetMaterialPreviewCameraFov(previewId, radians));

    public static void SetRotationSpeed(uint previewId, float radPerSec) =>
        Safe(() => _SetMaterialPreviewRotationSpeed(previewId, radPerSec));

    public static void SetLightDirection(uint previewId, float x, float y, float z)
    {
        var v = new[] { x, y, z };
        var pin = GCHandle.Alloc(v, GCHandleType.Pinned);
        try { Safe(() => _SetMaterialPreviewLightDirection(previewId, pin.AddrOfPinnedObject())); }
        finally { pin.Free(); }
    }

    public static void SetLightColor(uint previewId, float r, float g, float b)
    {
        var v = new[] { r, g, b };
        var pin = GCHandle.Alloc(v, GCHandleType.Pinned);
        try { Safe(() => _SetMaterialPreviewLightColor(previewId, pin.AddrOfPinnedObject())); }
        finally { pin.Free(); }
    }

    public static void SetLightIntensity(uint previewId, float intensity) =>
        Safe(() => _SetMaterialPreviewLightIntensity(previewId, intensity));

    public static void SetBackgroundMode(uint previewId, uint mode) =>
        Safe(() => _SetMaterialPreviewBackgroundMode(previewId, mode));

    public static void SetBackgroundColors(uint previewId, float topR, float topG, float topB, float botR, float botG, float botB)
    {
        var top = new[] { topR, topG, topB };
        var bot = new[] { botR, botG, botB };
        var topPin = GCHandle.Alloc(top, GCHandleType.Pinned);
        var botPin = GCHandle.Alloc(bot, GCHandleType.Pinned);
        try { Safe(() => _SetMaterialPreviewBackgroundColors(previewId, topPin.AddrOfPinnedObject(), botPin.AddrOfPinnedObject())); }
        finally { topPin.Free(); botPin.Free(); }
    }

    /// <summary>
    /// One-shot synchronous render: render preview into color_target on a
    /// graphics cmd buffer, copy to a readback buffer, BGRA→RGBA swap, copy
    /// into <paramref name="outRgba"/>. Buffer must be at least width*height*4 bytes.
    /// Returns bytes written (= width*height*4) on success, 0 on failure.
    /// </summary>
    public static ulong RenderToRGBA(uint previewId, ulong deviceHandle, byte[] outRgba)
    {
        if (outRgba == null || outRgba.Length == 0) return 0;
        var pin = GCHandle.Alloc(outRgba, GCHandleType.Pinned);
        try { return Safe(() => _RenderMaterialPreviewToRGBA(previewId, deviceHandle, pin.AddrOfPinnedObject(), (ulong)outRgba.Length)); }
        finally { pin.Free(); }
    }

    private static void Safe(Action a)
    {
        try { a(); } catch (DllNotFoundException) { /* engine not loaded */ }
        catch (Exception ex) { Logger.Warning($"MaterialPreviewNative call failed: {ex.Message}"); }
    }

    private static T Safe<T>(Func<T> f)
    {
        try { return f(); }
        catch (DllNotFoundException) { return default!; }
        catch (Exception ex) { Logger.Warning($"MaterialPreviewNative call failed: {ex.Message}"); return default!; }
    }
}