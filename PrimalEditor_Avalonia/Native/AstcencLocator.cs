using System;
using System.IO;
using System.Runtime.InteropServices;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// Resolves the astcenc CLI binary used for ASTC texture compression /
/// decompression in the ContentTools Metal cook and in TEXR previews.
/// Prefers PRIMAL_ASTCENC_PATH, then probes common locations.
/// </summary>
public static class AstcencLocator
{
    private static string? _cached;

    // .NET's Environment.SetEnvironmentVariable does not reliably make the
    // value visible to native getenv() on macOS; the ContentTools cook calls
    // getenv("PRIMAL_ASTCENC_PATH") to locate astcenc. Use libc setenv so the
    // native side sees it.
    [DllImport("libc", EntryPoint = "setenv", SetLastError = true)]
    private static extern int SetEnv(string name, string value, int overwrite);

    public static string? Resolve()
    {
        if (_cached != null) return _cached;

        var env = Environment.GetEnvironmentVariable("PRIMAL_ASTCENC_PATH");
        if (!string.IsNullOrEmpty(env) && File.Exists(env))
        {
            return _cached = env;
        }

        string baseDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(baseDir, "astcenc-neon"),
            Path.Combine(baseDir, "..", "astcenc-neon"),
            Path.Combine(baseDir, "..", "..", "third_party", "astc-encoder", "build", "Source", "astcenc-neon"),
            Path.Combine(baseDir, "..", "..", "..", "third_party", "astc-encoder", "build", "Source", "astcenc-neon"),
        };

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return _cached = candidate;
            }
        }

        return _cached = null;
    }

    /// <summary>
    /// Make the resolved astcenc path visible to native getenv() via libc
    /// setenv. Returns the resolved path (or null if none was found).
    /// </summary>
    public static string? EnsureExported()
    {
        var path = Resolve();
        if (path == null) return null;
        SetEnv("PRIMAL_ASTCENC_PATH", path, 1);
        return path;
    }
}

