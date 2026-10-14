using System;
using System.Threading.Tasks;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Editor-side host of the command channel. Wires the CliFramework registry
/// to the Avalonia UI thread and owns the socket server lifecycle. Failure
/// policy: the CLI channel must never take the editor down — every entry
/// point catches and degrades to a log line.
/// </summary>
public static class CliHost
{
    /// <summary>The registry shared by the socket server and editor.schema.
    /// Registering happens once in <see cref="Start"/> before the server
    /// accepts, so readers never see a partially built registry.</summary>
    public static CliCommandRegistry Registry { get; } = new();

    private static CliServer? _server;

    public static bool IsRunning => _server != null;

    /// <summary>Call once during editor startup (App.Initialize). Safe to call
    /// repeatedly; disabled by setting PRIMAL_CLI_DISABLE=1.</summary>
    public static void Start()
    {
        if (_server != null) return;
        if (Environment.GetEnvironmentVariable("PRIMAL_CLI_DISABLE") == "1") return;

        try
        {
            Registry.Register(new EditorCommands());
            Registry.Register(new EntityCommands());
            Registry.Register(new SceneCommands());
            Registry.Register(new ComponentCommands());
            Registry.Register(new PhysicsCommands());
            Registry.Register(new ScriptCommands());
            Registry.Register(new MaterialCommands());
            Registry.Register(new FrameCaptureCommands());
            Registry.Register(new SequenceCommands());
            Registry.Register(new RunCommands());
            Registry.Register(new TaskCommands());
            Registry.Register(new WorkflowCommands(Registry));
            Registry.Register(new DiagnosticsCommands());
            Registry.Register(new IntegrationCommands());
            Registry.Register(new PrefabCommands());
            Registry.Register(new DeviceDataCommands());

            var server = new CliServer(Registry, MarshalToUiThread, log: message => Logger.Debug(message));
            server.Start(CliServer.DefaultSocketPath());
            _server = server;

            EngineLogBridge.Install();
            Logger.Info($"[cli] 命令通道就绪: {server.SocketPath}");
        }
        catch (Exception ex)
        {
            Logger.Warning($"[cli] 命令通道启动失败（编辑器继续运行）: {ex.Message}");
            try { _server?.Stop(); } catch { }
            _server = null;
        }
    }

    /// <summary>Idempotent; called from App.OnExit before engine teardown.</summary>
    public static void Stop()
    {
        try { _server?.Stop(); } catch { }
        _server = null;
        EngineLogBridge.Uninstall();
    }

    /// <summary>
    /// Runs command handlers on the Avalonia UI thread — the same thread that
    /// owns Scene/GameEntity and drives the render loop (plan §9 risk #1).
    /// Built on InvokeAsync + explicit TaskCompletionSource so the returned
    /// task completes only when the handler's async work finished (the
    /// naive `await InvokeAsync(async () => …)` returns as soon as the
    /// delegate *returns*, not when its work completes).
    /// </summary>
    private static Task MarshalToUiThread(Func<Task> work)
    {
        var tcs = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
        _ = Dispatcher.UIThread.InvokeAsync(async () =>
        {
            try
            {
                await work().ConfigureAwait(true);
                tcs.SetResult(null);
            }
            catch (Exception ex)
            {
                tcs.SetException(ex);
            }
        });
        return tcs.Task;
    }
}
