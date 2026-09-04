using System;
using System.IO;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Editor-side façade for running the engine build script. Resolves the
/// build command via <see cref="IBuildCommandResolver"/> and launches
/// it through <see cref="IProcessRunner"/>, so production code never
/// hardcodes a shell binary and tests can substitute a fake runner.
///
/// Public events (<see cref="OnOutput"/>, <see cref="OnCompleted"/>)
/// and entry points (<see cref="Start"/>, <see cref="StartRebuild"/>,
/// <see cref="StartApp"/>, <see cref="Stop"/>) are preserved from the
/// pre-refactor version so the existing MainWindow / build log view
/// keep working.
/// </summary>
public static class BuildService
{
    private static IProcessRunner _runner = new SystemProcessRunner();
    private static IBuildCommandResolver _resolver = new PlatformBuildCommandResolver();
    private static IProcessHandle? _currentHandle;
    private static int _currentExitCode;
    private static bool _currentExited;

    public static bool IsRunning => _currentHandle != null && !_currentExited;

    public static event Action<string>? OnOutput;
    public static event Action<int, bool>? OnCompleted;

    public static void Override(IBuildCommandResolver resolver, IProcessRunner runner)
    {
        _resolver = resolver ?? new PlatformBuildCommandResolver();
        _runner = runner ?? new SystemProcessRunner();
    }

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

    public static bool Start(string config = "DebugEditor") => RunAction(BuildAction.Build, config);

    public static bool StartRebuild(string config = "DebugEditor") => RunAction(BuildAction.Rebuild, config);

    public static bool StartApp(string config = "DebugEditor")
    {
        if (IsRunning) return false;
        // App launch is conceptually a separate "run" verb on the
        // editor's launcher script. Until that script is split out we
        // reuse the same resolution pipeline so the platform-specific
        // shell choice is consistent.
        return RunAction(BuildAction.Run, config);
    }

    /// <summary>
    /// Launch the PrimalPlayer runtime host directly (plan §Run/Stop
    /// 2026-09-03). Unlike the script-based verbs this builds its own
    /// <see cref="BuildCommand"/> — the caller resolves the host binary
    /// and passes absolute arguments. Stop/IsRunning/OnOutput semantics
    /// are identical to the other verbs.
    /// </summary>
    public static bool StartPlayer(string executable, System.Collections.Generic.IReadOnlyList<string> arguments, string workingDirectory)
    {
        if (IsRunning) return false;
        if (string.IsNullOrEmpty(executable))
        {
            Logger.Error("BuildService: StartPlayer requires an executable path");
            return false;
        }
        try
        {
            _currentExitCode = 0;
            _currentExited = false;
            _currentHandle = _runner.Start(
                new BuildCommand(executable, arguments, workingDirectory),
                line => OnOutput?.Invoke(line));
            _currentHandle.SubscribeExit(code =>
            {
                _currentExited = true;
                _currentExitCode = code;
                var ok = code == 0;
                Logger.Info($"BuildService: player exited (code={code}, success={ok})");
                OnCompleted?.Invoke(code, ok);
            });
            Logger.Info($"BuildService: player started ({executable})");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"BuildService: StartPlayer failed — {ex.Message}", ex);
            _currentHandle = null;
            return false;
        }
    }

    public static void Stop()
    {
        if (_currentHandle == null) return;
        try { _currentHandle.Kill(); }
        catch (Exception ex) { Logger.Warning($"BuildService.Stop failed: {ex.Message}"); }
    }

    private static bool RunAction(BuildAction action, string config)
    {
        if (IsRunning) return false;
        if (string.IsNullOrEmpty(config))
        {
            Logger.Error("BuildService: empty config");
            return false;
        }
        if (!_resolver.TryResolve(action, config, out var command))
        {
            Logger.Error($"BuildService: {action} not supported on this platform");
            return false;
        }
        try
        {
            _currentExitCode = 0;
            _currentExited = false;
            _currentHandle = _runner.Start(command, line => OnOutput?.Invoke(line));
            _currentHandle.SubscribeExit(code =>
            {
                _currentExited = true;
                _currentExitCode = code;
                var ok = code == 0;
                Logger.Info($"BuildService: process exited (code={code}, success={ok})");
                OnCompleted?.Invoke(code, ok);
            });
            Logger.Info($"BuildService: started (action={action}, config={config})");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"BuildService: {action} failed — {ex.Message}", ex);
            _currentHandle = null;
            return false;
        }
    }
}
