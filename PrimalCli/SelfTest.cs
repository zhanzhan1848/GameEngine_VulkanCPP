using System;
using System.Text.Json;
using System.Threading.Tasks;
using Primal.CliFramework;

namespace PrimalCli;

/// <summary>
/// In-process roundtrip of the whole framework stack — registry reflection,
/// param binding, error packaging, schema generation, length-prefix framing,
/// socket server + client — without needing a running editor. This is the
/// fast lane of the CI gate for the CLI framework itself.
/// </summary>
internal static class SelfTest
{
    private static int _failures;

    public static int RunAll()
    {
        TestRegistryBinding();
        TestRegistryErrors();
        TestSchemaVersioning();
        TestFailFast();
        TestServerRoundtrip().GetAwaiter().GetResult();

        if (_failures > 0)
        {
            Console.Error.WriteLine($"selftest: {_failures} failure(s)");
            return 1;
        }
        Console.Error.WriteLine("selftest: all passed");
        return 0;
    }

    private static void Check(bool condition, string what)
    {
        if (condition)
        {
            Console.Error.WriteLine($"  ok  {what}");
        }
        else
        {
            _failures++;
            Console.Error.WriteLine($"FAIL  {what}");
        }
    }

    private sealed class TestCommands
    {
        [CliCommand("t.add", Description = "add", Example = "{\"a\":1,\"b\":2}")]
        public static object Add(int a, int b = 10) => new { sum = a + b };

        [CliCommand("t.vec", Description = "vec")]
        public static Vec3 Vec(Vec3 v) => v;

        [CliCommand("t.fail", Description = "cli error")]
        public static object Fail(string mode) => mode switch
        {
            "notfound" => throw CliError.NotFound("entity xyz", "用 scene.tree 查询"),
            "boom" => throw new InvalidOperationException("unexpected"),
            _ => throw CliError.InvalidParams("bad mode"),
        };

        [CliCommand("t.async", Description = "async")]
        public static async Task<object> Async(int delayMs) { await Task.Delay(delayMs); return new { done = true }; }
    }

    private static CliCommandRegistry NewRegistry()
    {
        var registry = new CliCommandRegistry();
        registry.RegisterStatic(typeof(TestCommands));
        return registry;
    }

    private static void TestRegistryBinding()
    {
        var registry = NewRegistry();

        var r1 = registry.InvokeAsync(new CliRequest { Id = "1", Verb = "t.add", Params = Json("""{"a":2}""") }).Result;
        Check(r1.Ok && r1.Data!.Value.GetProperty("sum").GetInt32() == 12, "缺省可选参数用默认值 (a=2,b=10 → 12)");

        var r2 = registry.InvokeAsync(new CliRequest { Id = "2", Verb = "t.add", Params = Json("""{"a":1,"b":2}""") }).Result;
        Check(r2.Ok && r2.Data!.Value.GetProperty("sum").GetInt32() == 3, "全参数绑定");

        var r3 = registry.InvokeAsync(new CliRequest { Id = "3", Verb = "t.add" }).Result;
        Check(!r3.Ok && r3.Error!.Code == CliErrorCodes.InvalidParams
            && r3.Error.Message.Contains("a"), "缺必填参数 → INVALID_PARAMS 指名道姓");

        var r4 = registry.InvokeAsync(new CliRequest { Id = "4", Verb = "t.vec", Params = Json("""{"v":"1,2,3"}""") }).Result;
        Check(r4.Ok && r4.Data!.Value[0].GetSingle() == 1f, "Vec3 接受 \"x,y,z\" 字符串");

        var r5 = registry.InvokeAsync(new CliRequest { Id = "5", Verb = "t.vec", Params = Json("""{"v":[4,5,6]}""") }).Result;
        Check(r5.Ok && r5.Data!.Value[2].GetSingle() == 6f, "Vec3 接受数组，输出数组");
    }

    private static void TestRegistryErrors()
    {
        var registry = NewRegistry();

        var r1 = registry.InvokeAsync(new CliRequest { Id = "e1", Verb = "t.fail", Params = Json("""{"mode":"notfound"}""") }).Result;
        Check(!r1.Ok && r1.Error!.Code == CliErrorCodes.NotFound && r1.Error.Hint != null, "CliError.NotFound 带 hint");

        var r2 = registry.InvokeAsync(new CliRequest { Id = "e2", Verb = "t.fail", Params = Json("""{"mode":"boom"}""") }).Result;
        Check(!r2.Ok && r2.Error!.Code == CliErrorCodes.Internal, "未预期异常包装为 INTERNAL");

        var r3 = registry.InvokeAsync(new CliRequest { Id = "e3", Verb = "nope.verb" }).Result;
        Check(!r3.Ok && r3.Error!.Code == CliErrorCodes.BadVerb, "未知 verb → BAD_VERB");

        var r4 = registry.InvokeAsync(new CliRequest { Id = "e4", Verb = "t.async", Params = Json("""{"delayMs":5}""") }).Result;
        Check(r4.Ok && r4.Data!.Value.GetProperty("done").GetBoolean(), "async handler 等待完成");
    }

    private static void TestSchemaVersioning()
    {
        var a = NewRegistry();
        var b = NewRegistry();
        Check(a.ComputeVersion() == b.ComputeVersion(), "同内容版本哈希一致");

        var schema = JsonSerializer.SerializeToElement(a.BuildSchema())!;
        Check(schema.GetProperty("$version").GetString()!.Length == 16, "schema 带 16 位 $version");
        Check(schema.GetProperty("commands").GetArrayLength() >= 4, "schema 列出全部命令");
    }

    private static void TestFailFast()
    {
        var threw = false;
        try
        {
            var registry = new CliCommandRegistry();
            registry.RegisterStatic(typeof(BadCommands));
        }
        catch (CliFrameworkException)
        {
            threw = true;
        }
        Check(threw, "无 schema 规则的参数类型 → 注册即抛 CliFrameworkException (fail-fast)");
    }

    private sealed class BadCommands
    {
        [CliCommand("t.bad")]
        public static object Bad(System.IO.FileInfo unsupported) => new();
    }

    private static async Task TestServerRoundtrip()
    {
        var registry = NewRegistry();
        var socketPath = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(), $"primal-selftest-{Guid.NewGuid():N}.sock");
        using var server = new CliServer(registry, work => work(), log: null);
        server.Start(socketPath);

        using var client = new CliClient();
        client.Connect(socketPath);
        var resp = await client.InvokeAsync("t.add", Json("""{"a":40,"b":2}"""));
        Check(resp.Ok && resp.Data!.Value.GetProperty("sum").GetInt32() == 42
            && resp.Meta != null, "socket 往返: 请求→UI线程编组→响应→meta");

        // Two sequential requests on one connection keep framing in sync.
        var resp2 = await client.InvokeAsync("t.fail", Json("""{"mode":"notfound"}"""));
        Check(!resp2.Ok && resp2.Error!.Code == CliErrorCodes.NotFound, "同一连接第二个请求帧同步正确");

        var resp3 = await client.InvokeAsync("t.add", Json("[1,2,3]"));
        Check(!resp3.Ok && resp3.Error!.Code == CliErrorCodes.InvalidParams, "非对象 params → INVALID_PARAMS");
    }

    private static JsonElement Json(string raw) => JsonSerializer.Deserialize<JsonElement>(raw);
}
