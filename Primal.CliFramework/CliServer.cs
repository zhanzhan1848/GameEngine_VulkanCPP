using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Primal.CliFramework;

/// <summary>
/// Host-side command server. Listens on a local Unix domain socket, decodes
/// CliRequests and executes each one through the host-supplied marshaler —
/// the editor passes the Avalonia UI-thread dispatcher so handlers never
/// need to know about threading (plan §9 risk #1).
///
/// Safety contract: nothing here may take the host down. Every failure is
/// converted into a CliResponse; the accept loop survives client crashes;
/// Stop() is idempotent and safe to call from any thread.
/// </summary>
public sealed class CliServer : IDisposable
{
    /// <summary>Executes work on the host's command thread (editor: UI thread
    /// via Dispatcher). Must serialize with whatever else runs there.</summary>
    public delegate Task MainThreadMarshaler(Func<Task> work);

    private readonly CliCommandRegistry _registry;
    private readonly MainThreadMarshaler _marshal;
    private readonly Action<string>? _log;
    private Socket? _listener;
    private string? _socketPath;
    private CancellationTokenSource? _cts;
    private int _disposed;

    public CliServer(CliCommandRegistry registry, MainThreadMarshaler marshal, Action<string>? log = null)
    {
        _registry = registry ?? throw new ArgumentNullException(nameof(registry));
        _marshal = marshal ?? throw new ArgumentNullException(nameof(marshal));
        _log = log;
    }

    /// <summary>Socket path actually bound (set by Start).</summary>
    public string? SocketPath => _socketPath;

    /// <summary>
    /// Default socket location per plan §6.1: ~/.primal/editor-&lt;pid&gt;.sock.
    /// The pid suffix lets several editor instances coexist; clients discover
    /// them via <see cref="CliClient.DiscoverSocketPaths"/>.
    /// </summary>
    public static string DefaultSocketPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".primal", $"editor-{Environment.ProcessId}.sock");

    public void Start(string socketPath)
    {
        if (Interlocked.CompareExchange(ref _disposed, 0, 0) != 0)
            throw new ObjectDisposedException(nameof(CliServer));
        if (_listener != null)
            throw new InvalidOperationException("CliServer already started");

        var dir = Path.GetDirectoryName(socketPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        // A stale file from a crashed editor would make bind() fail; remove
        // it only if nothing is listening (a live server keeps accepting).
        if (File.Exists(socketPath))
        {
            if (TryConnectStale(socketPath, out var live))
            {
                throw new InvalidOperationException(
                    $"socket '{socketPath}' 已被活跃的编辑器占用 (pid {live})");
            }
            TryDeleteSocketFile(socketPath);
        }

        var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        listener.Bind(new UnixDomainSocketEndPoint(socketPath));
        listener.Listen(8);

        _cts = new CancellationTokenSource();
        _socketPath = socketPath;
        _listener = listener;
        _log?.Invoke($"[cli] command socket listening: {socketPath}");
        _ = Task.Run(() => AcceptLoopAsync(listener, _cts.Token));
    }

    private static bool TryConnectStale(string path, out int connectedPid)
    {
        connectedPid = 0;
        try
        {
            using var probe = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            probe.Connect(new UnixDomainSocketEndPoint(path));
            return probe.Connected; // someone is listening → not stale
        }
        catch
        {
            return false;
        }
    }

    private static void TryDeleteSocketFile(string path)
    {
        try { File.Delete(path); } catch { /* best effort */ }
    }

    private async Task AcceptLoopAsync(Socket listener, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            Socket client;
            try
            {
                client = await listener.AcceptAsync(ct).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { break; }
            catch (SocketException) when (ct.IsCancellationRequested) { break; }
            catch (ObjectDisposedException) { break; }
            catch (Exception ex)
            {
                _log?.Invoke($"[cli] accept failed, retrying: {ex.Message}");
                try { await Task.Delay(200, ct).ConfigureAwait(false); }
                catch (OperationCanceledException) { break; }
                continue;
            }

            // One task per connection; connections are independent so a stuck
            // client cannot wedge the editor (UI-thread work is marshaled and
            // serialized there, network I/O never touches the UI thread).
            _ = Task.Run(() => ServeConnectionAsync(client, ct), ct);
        }
    }

    private async Task ServeConnectionAsync(Socket client, CancellationToken ct)
    {
        using var _ = client;
        await using var stream = new NetworkStream(client, ownsSocket: false);
        try
        {
            while (!ct.IsCancellationRequested)
            {
                byte[] frame;
                try
                {
                    frame = await FrameIO.ReadFrameAsync(stream, ct).ConfigureAwait(false);
                }
                catch (EndOfStreamException) { break; }          // clean disconnect
                catch (OperationCanceledException) { break; }
                catch (IOException) { break; }                    // reset / broken pipe

                var receivedAt = DateTime.UtcNow;
                CliRequest request;
                try
                {
                    request = frame.Length == 0
                        ? new CliRequest()
                        : JsonSerializer.Deserialize<CliRequest>(frame, CliCommandRegistry.JsonOptions) ?? new CliRequest();
                }
                catch (JsonException je)
                {
                    await WriteResponseAsync(stream, new CliResponse
                    {
                        Ok = false,
                        Error = new() { Code = CliErrorCodes.BadRequest, Message = $"请求 JSON 无效: {je.Message}" },
                    }).ConfigureAwait(false);
                    continue;
                }

                await DispatchAsync(stream, request, receivedAt).ConfigureAwait(false);
            }
        }
        catch (Exception ex)
        {
            _log?.Invoke($"[cli] connection handler ended: {ex.Message}");
        }
    }

    /// <summary>Marshal handler execution onto the host thread, measure queue
    /// latency, then write the response from the network side.</summary>
    private async Task DispatchAsync(Stream stream, CliRequest request, DateTime receivedAt)
    {
        // Pre-assigned so a misbehaving marshaler (never runs the work)
        // still produces a response instead of an NRE on the wire.
        var response = new CliResponse
        {
            Id = request.Id,
            Ok = false,
            Error = new() { Code = CliErrorCodes.Internal, Message = "host dispatch did not execute" },
        };
        try
        {
            await _marshal(async () =>
            {
                response = await _registry.InvokeAsync(request).ConfigureAwait(true);
            }).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            // Marshaler itself failed (e.g. dispatcher shut down).
            response = new CliResponse
            {
                Id = request.Id,
                Ok = false,
                Error = new() { Code = CliErrorCodes.Internal, Message = $"host dispatch failed: {ex.Message}" },
            };
        }

        response.Meta = new CliResponseMeta
        {
            QueueMs = Math.Round((DateTime.UtcNow - receivedAt).TotalMilliseconds, 2),
            ExecMs = response.Meta?.ExecMs ?? 0,
        };
        await WriteResponseAsync(stream, response).ConfigureAwait(false);
    }

    private static async Task WriteResponseAsync(Stream stream, CliResponse response)
    {
        var json = JsonSerializer.Serialize(response, CliCommandRegistry.JsonOptions);
        var frame = FrameIO.EncodeFrame(json);
        await stream.WriteAsync(frame).ConfigureAwait(false);
        await stream.FlushAsync().ConfigureAwait(false);
    }

    /// <summary>Idempotent; safe from any thread. Removes the socket file.</summary>
    public void Stop()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        try { _cts?.Cancel(); } catch { }
        try { _listener?.Dispose(); } catch { }
        if (_socketPath != null) TryDeleteSocketFile(_socketPath);
        _log?.Invoke("[cli] command socket stopped");
    }

    public void Dispose() => Stop();
}
