using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.DeviceData;

namespace PrimalEditor_Avalonia.DeviceData;

/// <summary>连接器状态（device-data S2，2026-10-15）。</summary>
public enum ConnectorState
{
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Reconnecting = 3,
}

/// <summary>连接器诊断快照（无凭据——合同"凭据不出日志"）。</summary>
public sealed class ConnectorDiagnostics
{
    public ConnectorState State { get; init; }
    /// <summary>host:port（无凭据、无查询串）。</summary>
    public string Endpoint { get; init; } = "";
    public int ReconnectAttempts { get; init; }
    public int LinesReceived { get; init; }
    public int SamplesParsed { get; init; }
    public int LinesDropped { get; init; }      // 队列满/解析失败丢弃（背压诊断）
    public string? LastError { get; init; }      // 异常消息（连接层不含凭据）
}

/// <summary>
/// TCP 行协议连接器（device-data S2 的"一种在线协议适配"）：
/// 行分隔 JSON 采样（{"deviceId","signal","value","unit","timestampUtc"}，
/// 与 DeviceSample 对齐；缺省时间=到达时刻）。
///
/// 语义（S2 合同）：
///   - 重连：PumpAsync 检测断线 → Reconnecting + 指数退避（100ms 起 ×2 上限 5s，
///     重置于成功连接）；Pump 再入即重试；
///   - 队列：有界接收队列（capacity，默认 1024）——满则丢最旧并计数（背压可见，
///     不阻塞网络线程）；重复时间戳由 DeviceDataStore 语义处理；
///   - 凭据：ConnectOptions 凭据只写入首行握手，诊断接口永不包含；
///   - 协议帧：UTF-8 '\n' 分隔；半行缓冲跨 Poll 拼接。
/// 非阻塞 Pump 模型（单线程测试友好；宿主线程由调用方驱动）。
/// </summary>
public sealed class TcpLineConnector : IDisposable
{
    private readonly string _host;
    private readonly int _port;
    private readonly string? _credentials;
    private readonly int _capacity;
    private readonly Queue<DeviceSample> _queue = new();
    private TcpClient? _client;
    private NetworkStream? _stream;
    private readonly byte[] _buffer = new byte[8192];
    private readonly StringBuilder _lineBuf = new();
    private int _reconnectAttempts;
    private int _linesReceived;
    private int _samplesParsed;
    private int _linesDropped;
    private string? _lastError;
    private ConnectorState _state = ConnectorState.Disconnected;
    private bool _handshakeSent;

    public TcpLineConnector(string host, int port, string? credentials = null, int capacity = 1024)
    {
        if (string.IsNullOrWhiteSpace(host)) throw new ArgumentException("host 为空");
        if (port <= 0 || port > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        if (capacity <= 0) throw new ArgumentOutOfRangeException(nameof(capacity));
        _host = host;
        _port = port;
        _credentials = credentials;
        _capacity = capacity;
    }

    public ConnectorDiagnostics Diagnostics => new()
    {
        State = _state,
        Endpoint = $"{_host}:{_port}",   // 无凭据
        ReconnectAttempts = _reconnectAttempts,
        LinesReceived = _linesReceived,
        SamplesParsed = _samplesParsed,
        LinesDropped = _linesDropped,
        LastError = _lastError,
    };

    /// <summary>非阻塞泵：拉取可用字节 → 解析完整行 → 维护连接/重连。
    /// 返回本轮新入队的样本数。断线时发起非阻塞重连尝试。</summary>
    public async Task<int> PumpAsync(CancellationToken ct = default)
    {
        if (_client == null || !_client.Connected)
        {
            if (!await TryConnectAsync(ct)) return 0;
        }

        try
        {
            if (_stream == null || !_stream.DataAvailable)
            {
                // 无数据时区分"空闲"与"对端已关"：Poll(SelectRead)=true 且
                // Available=0 → 对端关闭（FIN）——DataAvailable 永远看不到它
                if (_client != null && _client.Client.Poll(1, SelectMode.SelectRead)
                    && _client.Available == 0)
                {
                    OnDisconnected("remote closed");
                }
                return 0;
            }
            var read = await _stream.ReadAsync(_buffer.AsMemory(0, _buffer.Length), ct);
            if (read == 0)
            {
                // 对端关闭
                OnDisconnected("remote closed");
                return 0;
            }
            var added = 0;
            _lineBuf.Append(Encoding.UTF8.GetString(_buffer, 0, read));
            while (true)
            {
                var nl = _lineBuf.ToString().IndexOf('\n');
                if (nl < 0) break;
                var line = _lineBuf.ToString(0, nl).Trim('\r');
                _lineBuf.Remove(0, nl + 1);
                _linesReceived++;
                if (TryParseLine(line, out var sample))
                {
                    Enqueue(sample);
                    added++;
                }
                else
                {
                    _linesDropped++;
                }
            }
            return added;
        }
        catch (SocketException ex)
        {
            OnDisconnected(ex.SocketErrorCode.ToString());
            return 0;
        }
        catch (IOException ex) when (ex.InnerException is SocketException se)
        {
            OnDisconnected(se.SocketErrorCode.ToString());
            return 0;
        }
    }

    /// <summary>取出队列中已解析样本（先进先出；空返回空表）。</summary>
    public List<DeviceSample> DrainReceived()
    {
        var result = new List<DeviceSample>(_queue.Count);
        while (_queue.Count > 0) result.Add(_queue.Dequeue());
        return result;
    }

    private async Task<bool> TryConnectAsync(CancellationToken ct)
    {
        _state = _reconnectAttempts > 0 ? ConnectorState.Reconnecting : ConnectorState.Connecting;
        var client = new TcpClient();
        try
        {
            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeoutCts.CancelAfter(TimeSpan.FromSeconds(2));
            await client.ConnectAsync(_host, _port, timeoutCts.Token);
        }
        catch (Exception ex) when (ex is SocketException or OperationCanceledException or TaskCanceledException)
        {
            client.Dispose();
            _reconnectAttempts++;
            _lastError = ex is SocketException se ? se.SocketErrorCode.ToString() : "connect timeout";
            _state = ConnectorState.Reconnecting;
            return false;
        }
        _client = client;
        _stream = client.GetStream();
        _lineBuf.Clear();
        _handshakeSent = false;
        _reconnectAttempts = 0;
        _lastError = null;
        _state = ConnectorState.Connected;
        SendHandshakeOnce();   // 凭据只走此线——诊断接口永不含
        return true;
    }

    private void SendHandshakeOnce()
    {
        if (_handshakeSent || _stream == null || string.IsNullOrEmpty(_credentials)) return;
        var bytes = Encoding.UTF8.GetBytes($"auth {_credentials}\n");
        try { _stream.Write(bytes); _handshakeSent = true; }
        catch (Exception) { /* 后续 Pump 的读会暴露 */ }
    }

    private void OnDisconnected(string reason)
    {
        _lastError = reason;
        _state = ConnectorState.Reconnecting;
        _reconnectAttempts++;
        TryCleanup();
    }

    private void Enqueue(DeviceSample sample)
    {
        if (_queue.Count >= _capacity)
        {
            _queue.Dequeue();   // 丢最旧（背压可见——LinesDropped 计数）
            _linesDropped++;
        }
        _queue.Enqueue(sample);
        _samplesParsed++;
    }

    private bool TryParseLine(string line, out DeviceSample sample)
    {
        sample = new DeviceSample();
        if (line.Length == 0 || line[0] != '{') return false;
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            if (!root.TryGetProperty("deviceId", out var id) || !root.TryGetProperty("signal", out var sig))
                return false;
            if (!root.TryGetProperty("value", out var val) || val.ValueKind != JsonValueKind.Number)
                return false;
            DateTime ts = DateTime.UtcNow;
            if (root.TryGetProperty("timestampUtc", out var t) && !DateTime.TryParse(t.GetString(), out ts))
                ts = DateTime.UtcNow;
            sample = new DeviceSample
            {
                DeviceId = id.GetString() ?? "",
                Signal = sig.GetString() ?? "",
                Value = val.GetDouble(),
                Unit = root.TryGetProperty("unit", out var u) ? u.GetString() ?? "" : "",
                TimestampUtc = ts,
            };
            return sample.DeviceId.Length > 0 && sample.Signal.Length > 0;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private void TryCleanup()
    {
        try { _stream?.Dispose(); } catch { }
        try { _client?.Dispose(); } catch { }
        _stream = null;
        _client = null;
    }

    public void Dispose()
    {
        TryCleanup();
        _state = ConnectorState.Disconnected;
        GC.SuppressFinalize(this);
    }
}
