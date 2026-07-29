using System;
using System.Diagnostics;
using System.IO;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Wraps the engine build script (build_engine.sh) so the editor can
/// trigger a build without spawning a terminal. Streams stdout/stderr
/// to <see cref="OnOutput"/> for the build log, and raises
/// <see cref="OnCompleted"/> when the process exits.
/// </summary>
public static class BuildService
{
    private static Process? _currentProcess;

    public static bool IsRunning => _currentProcess != null && !_currentProcess.HasExited;

    public static event System.Action<string>? OnOutput;
    public static event System.Action<int, bool>? OnCompleted;

    public static string BuildScriptPath
    {
        get
        {
            var dir = Path.Combine(
                Path.GetDirectoryName(typeof(BuildService).Assembly.Location) ?? ".",
                "build");
            return Path.Combine(dir, "build_engine.sh");
        }
    }

    public static string RunScriptPath
    {
        get
        {
            var dir = Path.Combine(
                Path.GetDirectoryName(typeof(BuildService).Assembly.Location) ?? ".",
                "build");
            return Path.Combine(dir, "build_run.sh");
        }
    }

    public static bool StartRebuild(string config = "DebugEditor")
    {
        if (IsRunning) return false;
        var script = BuildScriptPath;
        if (!File.Exists(script))
        {
            Logger.Error($"BuildService: script not found: {script}");
            return false;
        }
        return StartProcess(script, "rebuild", config);
    }

    public static bool StartApp(string config = "DebugEditor")
    {
        if (IsRunning) return false;
        var script = RunScriptPath;
        if (!File.Exists(script))
        {
            Logger.Error($"BuildService: run script not found: {script}");
            return false;
        }
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "/bin/bash",
                ArgumentList = { script, config },
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            var p = Process.Start(psi);
            if (p == null) return false;
            p.OutputDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.EnableRaisingEvents = true;
            p.Exited += (_, _) =>
            {
                Logger.Info($"BuildService: app launcher exited (code={p.ExitCode})");
                OnCompleted?.Invoke(p.ExitCode, p.ExitCode == 0);
            };
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            Logger.Info($"BuildService: app launcher started (config={config})");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"BuildService.StartApp failed: {ex.Message}", ex);
            return false;
        }
    }

    private static bool StartProcess(string script, string action, string config)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "/bin/bash",
                ArgumentList = { script, action, config },
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            var p = Process.Start(psi);
            if (p == null) return false;
            _currentProcess = p;
            p.OutputDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.EnableRaisingEvents = true;
            p.Exited += (_, _) =>
            {
                var ok = p.ExitCode == 0;
                Logger.Info($"BuildService: process exited (code={p.ExitCode}, success={ok})");
                _currentProcess = null;
                OnCompleted?.Invoke(p.ExitCode, ok);
            };
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            Logger.Info($"BuildService: started (action={action}, config={config})");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"BuildService.StartProcess failed: {ex.Message}", ex);
            _currentProcess = null;
            return false;
        }
    }

    public static bool Start(string config = "DebugEditor")
    {
        if (IsRunning) return false;
        var script = BuildScriptPath;
        if (!File.Exists(script))
        {
            Logger.Error($"BuildService: script not found: {script}");
            return false;
        }
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "/bin/bash",
                ArgumentList = { script, config },
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            var p = Process.Start(psi);
            if (p == null) return false;
            _currentProcess = p;
            p.OutputDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (e.Data != null) OnOutput?.Invoke(e.Data); };
            p.EnableRaisingEvents = true;
            p.Exited += (_, _) =>
            {
                var ok = p.ExitCode == 0;
                Logger.Info($"BuildService: process exited (code={p.ExitCode}, success={ok})");
                _currentProcess = null;
                OnCompleted?.Invoke(p.ExitCode, ok);
            };
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            Logger.Info($"BuildService: started (config={config})");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"BuildService.Start failed: {ex.Message}", ex);
            _currentProcess = null;
            return false;
        }
    }

    public static void Stop()
    {
        if (_currentProcess == null) return;
        try
        {
            _currentProcess.Kill(true);
        }
        catch (Exception ex)
        {
            Logger.Warning($"BuildService.Stop failed: {ex.Message}");
        }
    }
}
