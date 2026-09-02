using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Primal.CliFramework;

/// <summary>
/// Client half of the command channel: discovers the running editor's socket,
/// sends one request frame and awaits the matching response. Used by the
/// primal CLI, the MCP bridge and CI smoke scripts — all consumers share this
/// routing/exit-code semantics so behavior never diverges.
/// </summary>
public sealed class CliClient : IDisposable
{
    public const int DefaultTimeoutMs = 30_000;

    private Socket? _socket;
    private NetworkStream? _stream;

    /// <summary>
    /// Live editor sockets under ~/.primal, newest first. Sockets are named
    /// editor-&lt;pid&gt;.sock; entries whose file no longer has a listener are
    /// filtered by the caller via ConnectAsync failure.
    /// </summary>
    public static IReadOnlyList<string> DiscoverSocketPaths()
    {
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".primal");
        if (!Directory.Exists(dir)) return Array.Empty<string>();

        return Directory.GetFiles(dir, "editor-*.sock")
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .ToArray();
    }

    public void Connect(string socketPath, int timeoutMs = 5_000)
    {
        var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            socket.Connect(new UnixDomainSocketEndPoint(socketPath));
            _socket = socket;
            _stream = new NetworkStream(socket, ownsSocket: false);
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Try every discovered socket until one answers. Returns the socket path
    /// used, or null when no editor is reachable (CLI maps that to exit 5).
    /// </summary>
    public string? ConnectToAnyEditor(int perSocketTimeoutMs = 2_000)
    {
        foreach (var path in DiscoverSocketPaths())
        {
            try
            {
                Connect(path, perSocketTimeoutMs);
                return path;
            }
            catch
            {
                Dispose();
            }
        }
        return null;
    }

    public async Task<CliResponse> InvokeAsync(string verb, JsonElement? parameters = null,
        int timeoutMs = DefaultTimeoutMs, CancellationToken ct = default)
    {
        if (_stream == null)
            throw new InvalidOperationException("Not connected");

        var request = new CliRequest
        {
            Id = Guid.NewGuid().ToString("N")[..12],
            Verb = verb,
            Params = parameters ?? JsonSerializer.SerializeToElement(new { }),
        };

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(timeoutMs);

        var json = JsonSerializer.Serialize(request, CliCommandRegistry.JsonOptions);
        await _stream.WriteAsync(FrameIO.EncodeFrame(json), timeoutCts.Token).ConfigureAwait(false);
        await _stream.FlushAsync(timeoutCts.Token).ConfigureAwait(false);

        var frame = await FrameIO.ReadFrameAsync(_stream, timeoutCts.Token).ConfigureAwait(false);
        return JsonSerializer.Deserialize<CliResponse>(frame, CliCommandRegistry.JsonOptions)
               ?? throw new IOException("Empty response frame");
    }

    /// <summary>One-shot helper: connect (discovering if needed), invoke, disconnect.
    /// Connection failures (dead pid, stale socket, explicit path unreachable)
    /// normalize to NotConnected so every client maps them to exit 5.</summary>
    public static async Task<(CliResponse Response, string? SocketPath)> InvokeOnceAsync(
        string verb, JsonElement? parameters = null, int timeoutMs = DefaultTimeoutMs,
        string? socketPath = null, CancellationToken ct = default)
    {
        using var client = new CliClient();
        string? path;
        if (socketPath != null)
        {
            try { client.Connect(socketPath); }
            catch
            {
                return (new CliResponse
                {
                    Ok = false,
                    Error = new CliErrorPayload
                    {
                        Code = CliErrorCodes.NotConnected,
                        Message = $"无法连接命令 socket: {socketPath}",
                        Hint = "确认该路径对应一个运行中的编辑器；或省略 --socket 自动发现",
                    },
                }, null);
            }
            path = socketPath;
        }
        else
        {
            path = client.ConnectToAnyEditor();
            if (path == null)
            {
                return (new CliResponse
                {
                    Ok = false,
                    Error = new CliErrorPayload
                    {
                        Code = CliErrorCodes.NotConnected,
                        Message = "没有找到运行中的编辑器命令通道",
                        Hint = "先启动 PrimalEditor_Avalonia；用 `primal discover` 列出已有 socket",
                    },
                }, null);
            }
        }
        return (await client.InvokeAsync(verb, parameters, timeoutMs, ct).ConfigureAwait(false), path);
    }

    public void Dispose()
    {
        _stream?.Dispose();
        _stream = null;
        _socket?.Dispose();
        _socket = null;
    }
}
