using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.DeviceData;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.DeviceDataNs;

/// <summary>
/// device-data S2 在线协议契约测试（本地 TCP 真实网络——非模拟直调）：
/// 收样解析（deviceId/signal/value/unit）、半行跨 Poll 拼接、坏行丢弃计数、
/// 断线→Reconnecting 诊断→重连成功续收、凭据不出现在诊断、有界队列溢出
/// 丢最旧+背压计数、推入 DeviceDataStore 的乱序/重复语义复用。
/// </summary>
public sealed class TcpLineConnectorTests : IDisposable
{
    private sealed class FeedServer : IDisposable
    {
        private TcpListener _listener;
        private TcpClient? _client;
        public string ReceivedAuth = "";

        public FeedServer()
        {
            _listener = new TcpListener(IPAddress.Loopback, 0);
            _listener.Start();
        }

        public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

        public async Task AcceptAndReadAuthAsync()
        {
            _client = await _listener.AcceptTcpClientAsync();
            var buf = new byte[256];
            var stream = _client.GetStream();
            var readTask = stream.ReadAsync(buf, 0, buf.Length);
            if (await Task.WhenAny(readTask, Task.Delay(2000)) == readTask)
                ReceivedAuth = Encoding.UTF8.GetString(buf, 0, readTask.Result).Trim();
        }

        public void Send(string text)
        {
            _client?.GetStream().Write(Encoding.UTF8.GetBytes(text));
        }

        public void CloseClient()
        {
            _client?.Close();
            _client = null;
        }

        public void Dispose()
        {
            CloseClient();
            _listener.Stop();
        }
    }

    private static async Task<int> PumpUntil(TcpLineConnector connector, Func<bool> done,
        int maxRounds = 200)
    {
        var total = 0;
        for (var i = 0; i < maxRounds && !done(); i++)
        {
            total += await connector.PumpAsync();
            await Task.Delay(5);
        }
        return total;
    }

    [Fact]
    public async Task ReceivesParsesAndHalfLineAssembly()
    {
        using var server = new FeedServer();
        using var connector = new TcpLineConnector("127.0.0.1", server.Port, credentials: "secret-token");
        var acceptTask = server.AcceptAndReadAuthAsync();
        Assert.Equal(0, await connector.PumpAsync());   // 首轮连上（无数据）
        await acceptTask;
        Assert.Contains("secret-token", server.ReceivedAuth);   // 握手送达服务端

        // 半行拆发：行协议跨 Poll 拼接
        server.Send("""{"deviceId":"d1","signal":"temp","value":21.5,"unit":"degC"}"""[..40]);
        await Task.Delay(20);
        Assert.Equal(0, await connector.PumpAsync());   // 半行不入队
        server.Send("""{"deviceId":"d1","signal":"temp","value":21.5,"unit":"degC"}"""[40..] + "\n");
        await PumpUntil(connector, () => connector.Diagnostics.SamplesParsed >= 1);

        // 第二条坏行 + 好行
        server.Send("not-json\n{\"deviceId\":\"d2\",\"signal\":\"p\",\"value\":5}\n");
        await PumpUntil(connector, () => connector.Diagnostics.LinesReceived >= 3);

        var samples = connector.DrainReceived();
        Assert.Equal(2, samples.Count);
        Assert.Equal("d1", samples[0].DeviceId);
        Assert.Equal(21.5, samples[0].Value);
        Assert.Equal("degC", samples[0].Unit);
        Assert.Equal("d2", samples[1].DeviceId);

        var diag = connector.Diagnostics;
        Assert.Equal(3, diag.LinesReceived);       // 3 行到达
        Assert.Equal(1, diag.LinesDropped);        // 坏行丢弃计数
        Assert.Equal(ConnectorState.Connected, diag.State);
        Assert.DoesNotContain("secret", diag.Endpoint);   // 凭据不出诊断
        Assert.Null(diag.LastError);
    }

    [Fact]
    public async Task Disconnect_Reconnects_AndResumes()
    {
        using var server = new FeedServer();
        using var connector = new TcpLineConnector("127.0.0.1", server.Port);
        Assert.Equal(0, await connector.PumpAsync());
        var accept1 = server.AcceptAndReadAuthAsync();

        server.CloseClient();   // 对端断开
        await PumpUntil(connector, () => connector.Diagnostics.State == ConnectorState.Reconnecting);
        var diag = connector.Diagnostics;
        Assert.Equal(ConnectorState.Reconnecting, diag.State);
        Assert.NotNull(diag.LastError);

        // 重连（服务端重新接受）
        Assert.Equal(0, await connector.PumpAsync());
        await server.AcceptAndReadAuthAsync();
        await PumpUntil(connector, () => connector.Diagnostics.State == ConnectorState.Connected);
        Assert.Equal(ConnectorState.Connected, connector.Diagnostics.State);
        Assert.Equal(0, connector.Diagnostics.ReconnectAttempts);   // 成功后清零
    }

    [Fact]
    public async Task CredentialsNeverInDiagnostics()
    {
        using var server = new FeedServer();
        var secret = "top-secret-key-99";
        using var connector = new TcpLineConnector("127.0.0.1", server.Port, credentials: secret);
        await connector.PumpAsync();
        await server.AcceptAndReadAuthAsync();

        // 各状态快照均不含凭据
        var diag = connector.Diagnostics;
        Assert.DoesNotContain(secret, diag.Endpoint);
        Assert.DoesNotContain(secret, diag.LastError ?? "");

        server.CloseClient();
        await PumpUntil(connector, () => connector.Diagnostics.State == ConnectorState.Reconnecting);
        Assert.DoesNotContain(secret, connector.Diagnostics.LastError ?? "");
    }

    [Fact]
    public async Task BoundedQueue_DropsOldest_CountsBackpressure()
    {
        using var server = new FeedServer();
        using var connector = new TcpLineConnector("127.0.0.1", server.Port, capacity: 4);
        await connector.PumpAsync();
        await server.AcceptAndReadAuthAsync();

        // 发 8 条（容量 4 → 丢 4 最旧）
        var sb = new StringBuilder();
        for (var i = 0; i < 8; i++)
            sb.AppendLine($"{{\"deviceId\":\"d\",\"signal\":\"s{i}\",\"value\":{i}}}");
        server.Send(sb.ToString());
        await PumpUntil(connector, () => connector.Diagnostics.LinesReceived >= 8);

        var samples = connector.DrainReceived();
        Assert.Equal(4, samples.Count);                       // 容量上限
        Assert.Equal("s4", samples[0].Signal);                // 最旧的 s0..s3 被丢
        Assert.Equal("s7", samples[3].Signal);
        Assert.Equal(4, connector.Diagnostics.LinesDropped);  // 背压可见
    }

    [Fact]
    public async Task StoreIntegration_DuplicateAndOrderingSemantics()
    {
        using var server = new FeedServer();
        using var connector = new TcpLineConnector("127.0.0.1", server.Port);
        await connector.PumpAsync();
        await server.AcceptAndReadAuthAsync();

        var baseTime = DateTime.UtcNow;
        // 乱序 + 重复时间戳
        server.Send(
            $"{{\"deviceId\":\"d\",\"signal\":\"t\",\"value\":3,\"timestampUtc\":\"{baseTime.AddSeconds(2):o}\"}}\n" +
            $"{{\"deviceId\":\"d\",\"signal\":\"t\",\"value\":1,\"timestampUtc\":\"{baseTime:o}\"}}\n" +
            $"{{\"deviceId\":\"d\",\"signal\":\"t\",\"value\":99,\"timestampUtc\":\"{baseTime:o}\"}}\n");
        await PumpUntil(connector, () => connector.Diagnostics.LinesReceived >= 3);

        var store = new DeviceDataStore();
        store.Push(connector.DrainReceived());

        // S1 store 语义：新样本→Accepted；乱序→OutOfOrder（入史不占 latest）；
        // 重复时间戳→Duplicate 丢弃
        Assert.Equal(1, store.AcceptedCount);
        Assert.Equal(1, store.OutOfOrderCount);
        Assert.Equal(1, store.DuplicateCount);
        var latest = store.Latest("d", "t", baseTime.AddSeconds(3));
        Assert.Equal(3, latest.Sample.Value);                // latest=时间最大者（值 3），非 99 非 1
    }

    public void Dispose() { }
}
