using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// One resolved build command. The runner launches
/// <see cref="Executable"/> with the given <see cref="Arguments"/> in
/// <see cref="WorkingDirectory"/>. We never concatenate a shell command
/// string, so callers can't accidentally inject shell metacharacters.
/// </summary>
public sealed class BuildCommand
{
    public string Executable { get; }
    public IReadOnlyList<string> Arguments { get; }
    public string WorkingDirectory { get; }

    public BuildCommand(string executable, IReadOnlyList<string> arguments, string workingDirectory)
    {
        Executable = executable ?? string.Empty;
        Arguments = arguments ?? Array.Empty<string>();
        WorkingDirectory = workingDirectory ?? string.Empty;
    }
}

public enum BuildAction { Build, Rebuild, Run }

public enum HostPlatform { MacOS, Linux, Windows, Unknown }

public interface IPlatformProbe
{
    HostPlatform Current { get; }
}

public sealed class RuntimePlatformProbe : IPlatformProbe
{
    public HostPlatform Current
    {
        get
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX)) return HostPlatform.MacOS;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux)) return HostPlatform.Linux;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return HostPlatform.Windows;
            return HostPlatform.Unknown;
        }
    }
}

/// <summary>
/// Maps a logical (action, config) request to a platform-correct
/// <see cref="BuildCommand"/>. The default implementation honors the
/// current operating system — macOS uses the editor's bundled
/// <c>build_engine.sh</c> via <c>zsh</c>, Linux uses <c>bash</c>, and
/// Windows would resolve a future <c>build_engine.cmd</c> (out of
/// scope for the current macOS-first work). Tests inject their own
/// <see cref="IPlatformProbe"/> to cover additional platforms without
/// spawning them.
/// </summary>
public interface IBuildCommandResolver
{
    bool TryResolve(BuildAction action, string config, out BuildCommand command);
}

public sealed class PlatformBuildCommandResolver : IBuildCommandResolver
{
    private readonly IPlatformProbe _platform;
    private readonly string _editorBuildDir;

    public PlatformBuildCommandResolver()
        : this(new RuntimePlatformProbe(), ResolveEditorBuildDir()) { }
    public PlatformBuildCommandResolver(IPlatformProbe platform, string editorBuildDir)
    {
        _platform = platform;
        _editorBuildDir = editorBuildDir;
    }

    public bool TryResolve(BuildAction action, string config, out BuildCommand command)
    {
        command = default!;
        if (string.IsNullOrEmpty(config)) return false;
        var verb = action switch
        {
            BuildAction.Rebuild => "rebuild",
            BuildAction.Run => "run",
            _ => "build",
        };

        switch (_platform.Current)
        {
            case HostPlatform.MacOS:
            {
                var script = Path.Combine(_editorBuildDir, "build_engine.sh");
                command = new BuildCommand("/bin/zsh", new[] { script, verb, config }, _editorBuildDir);
                return true;
            }
            case HostPlatform.Linux:
            {
                var script = Path.Combine(_editorBuildDir, "build_engine.sh");
                command = new BuildCommand("/bin/bash", new[] { script, verb, config }, _editorBuildDir);
                return true;
            }
            // Windows returns false so the editor surfaces a clear
            // "platform not supported" error instead of silently
            // invoking the wrong shell.
            default:
                return false;
        }
    }

    private static string ResolveEditorBuildDir()
    {
        var asm = System.Reflection.Assembly.GetExecutingAssembly().Location;
        var asmDir = string.IsNullOrEmpty(asm)
            ? AppContext.BaseDirectory
            : Path.GetDirectoryName(asm) ?? AppContext.BaseDirectory;
        return Path.Combine(asmDir, "build");
    }
}
