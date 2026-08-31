using System;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// P/Invoke bindings for <c>EngineDLL/ShaderCompilerAPI.cpp</c> — compiles a
/// material graph to a Metal shader source string and applies it to a
/// registered shader slot via the standard render pipeline.
///
/// <see cref="Compile"/> requires the raw graph pointer obtained from
/// <see cref="MaterialGraphNative.GetRawPtr"/>; the C++ side casts back to
/// <c>material_graph::MaterialGraph*</c> internally.
/// </summary>
public static class ShaderCompilerNative
{
    private const string Lib = "PrimalEngine";

    public enum CompileStatus : uint
    {
        Failure = 0,
        Success = 1,
        CompileError = 2,
    }

    [DllImport(Lib, EntryPoint = "CompileMaterialShader", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CompileMaterialShader(
        IntPtr graphPtr,
        IntPtr outSource, ref uint ioSourceSize,
        IntPtr outError, uint errorBufSize);

    [DllImport(Lib, EntryPoint = "ApplyMaterialShader", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ApplyMaterialShader(ulong shaderHandle, IntPtr source, uint sourceSize);

    /// <summary>
    /// Compile <paramref name="graphPtr"/> to a Metal source string.
    /// On <see cref="CompileStatus.CompileError"/>, <paramref name="error"/>
    /// carries the diagnostic. The source buffer is grown automatically up
    /// to <paramref name="maxSourceSize"/> bytes (default 256 KiB).
    /// </summary>
    public static CompileStatus Compile(
        IntPtr graphPtr,
        int maxSourceSize,
        out string? source,
        out string? error)
    {
        source = null;
        error = null;
        if (graphPtr == IntPtr.Zero) return CompileStatus.Failure;

        var srcBytes = new byte[maxSourceSize];
        var errBytes = new byte[1024];
        var srcSize = (uint)maxSourceSize;
        var srcPin = GCHandle.Alloc(srcBytes, GCHandleType.Pinned);
        var errPin = GCHandle.Alloc(errBytes, GCHandleType.Pinned);
        try
        {
            CompileStatus status;
            try
            {
                status = (CompileStatus)_CompileMaterialShader(
                    graphPtr,
                    srcPin.AddrOfPinnedObject(), ref srcSize,
                    errPin.AddrOfPinnedObject(), (uint)errBytes.Length);
            }
            catch (DllNotFoundException) { status = CompileStatus.Failure; }
            catch (Exception ex) { Logger.Warning($"ShaderCompilerNative call failed: {ex.Message}"); status = CompileStatus.Failure; }

            if (status == CompileStatus.Success && srcSize > 0)
            {
                var len = (int)Math.Min(srcSize - 1, (uint)maxSourceSize - 1);
                source = System.Text.Encoding.UTF8.GetString(srcBytes, 0, len);
            }
            else if (status == CompileStatus.CompileError)
            {
                var errLen = 0;
                while (errLen < errBytes.Length && errBytes[errLen] != 0) errLen++;
                error = errLen == 0 ? "(no error message)" : System.Text.Encoding.UTF8.GetString(errBytes, 0, errLen);
            }
            return status;
        }
        finally
        {
            srcPin.Free(); errPin.Free();
        }
    }

    /// <summary>
    /// Hot-reload a registered shader slot with a new source string.
    /// Returns true if the standard render pipeline accepted the reload.
    /// </summary>
    public static bool Apply(ulong shaderHandle, string source)
    {
        if (string.IsNullOrEmpty(source) || shaderHandle == 0) return false;
        var bytes = System.Text.Encoding.UTF8.GetBytes(source);
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            return _ApplyMaterialShader(shaderHandle, pin.AddrOfPinnedObject(), (uint)bytes.Length) != 0;
        }
        catch (DllNotFoundException) { return false; }
        catch (Exception ex) { Logger.Warning($"ShaderCompilerNative call failed: {ex.Message}"); return false; }
        finally { pin.Free(); }
    }

    private static T Safe<T>(Func<T> f)
    {
        try { return f(); }
        catch (DllNotFoundException) { return default!; }
        catch (Exception ex) { Logger.Warning($"ShaderCompilerNative call failed: {ex.Message}"); return default!; }
    }
}