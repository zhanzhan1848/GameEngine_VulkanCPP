using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace Primal.CliFramework;

/// <summary>
/// Zero-hand-registration command registry: scans [CliCommand] methods via
/// reflection, binds JSON params by name, invokes handlers and generates the
/// JSON schema consumed by editor.schema / primal schema / MCP tool defs.
///
/// Threading: registration is single-threaded (host startup). InvokeAsync may
/// be called from any thread, but handlers run wherever the host's marshaler
/// puts them (the editor marshals everything onto the Avalonia UI thread).
/// </summary>
public sealed class CliCommandRegistry
{
    private readonly ConcurrentDictionary<string, CliCommandDescriptor> _commands = new(StringComparer.Ordinal);

    /// <summary>Shared serializer options. Handlers' return values serialize
    /// with these; parameter binding reuses them so custom converters (Vec3,
    /// enums, Guid) behave identically on the way in and out.</summary>
    public static JsonSerializerOptions JsonOptions { get; } = new()
    {
        PropertyNamingPolicy = null,
        WriteIndented = false,
        // 中文/特殊字符直出 UTF-8，不做 \uXXXX 转义——错误 hint 是写给 LLM 看的。
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        // LLM 宽进: enums bind from "Directional" as well as 0.
        Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter() },
    };

    public IReadOnlyCollection<CliCommandDescriptor> Commands => _commands.Values.OrderBy(c => c.Verb, StringComparer.Ordinal).ToArray();

    /// <summary>Register every [CliCommand] method of an object (instance
    /// handlers). The host keeps ownership of the target's lifetime.</summary>
    public void Register(object host) => RegisterHost(host.GetType(), host);

    /// <summary>Register static [CliCommand] methods of a type.</summary>
    public void RegisterStatic(Type type) => RegisterHost(type, null);

    private void RegisterHost(Type type, object? target)
    {
        var methods = type.GetMethods(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly);
        foreach (var method in methods)
        {
            var attr = method.GetCustomAttribute<CliCommandAttribute>();
            if (attr == null) continue;

            if (!attr.Verb.Contains('.') || attr.Verb.StartsWith('.') || attr.Verb.EndsWith('.'))
                throw new CliFrameworkException($"[CliCommand] '{attr.Verb}' ({type.Name}.{method.Name}) 必须使用点分命名空间形式，如 editor.ping");

            if (!_commands.TryAdd(attr.Verb, BuildDescriptor(attr, method, target)))
                throw new CliFrameworkException($"重复注册 verb '{attr.Verb}' ({type.Name}.{method.Name})");
        }
    }

    private static CliCommandDescriptor BuildDescriptor(CliCommandAttribute attr, MethodInfo method, object? target)
    {
        var parameters = method.GetParameters();
        var paramDescs = new CliParamDescriptor[parameters.Length];
        for (int i = 0; i < parameters.Length; i++)
        {
            var p = parameters[i];
            if (p.IsOut || p.ParameterType.IsByRef)
                throw new CliFrameworkException($"命令 {attr.Verb}: 参数 {p.Name} 不能是 out/ref");
            if (p.ParameterType == typeof(CancellationToken)) { paramDescs[i] = new CliParamDescriptor { Name = p.Name!, Type = "ct", Required = false }; continue; }
            paramDescs[i] = new CliParamDescriptor
            {
                Name = p.Name!,
                Type = DescribeType(p.ParameterType, attr.Verb, p.Name!),
                Required = !p.HasDefaultValue && Nullable.GetUnderlyingType(p.ParameterType) == null,
            };
        }

        var returnType = DescribeReturnType(method, attr.Verb);
        return new CliCommandDescriptor
        {
            Verb = attr.Verb,
            Description = attr.Description,
            Example = attr.Example,
            Channel = attr.Channel,
            Params = paramDescs,
            ReturnType = returnType,
            Method = method,
            Target = target,
        };
    }

    /// <summary>
    /// Schema rule whitelist. Anything not described here fails registration —
    /// add the type AND its schema mapping in the same commit.
    /// </summary>
    private static string DescribeType(Type t, string verb, string paramName)
    {
        var underlying = Nullable.GetUnderlyingType(t) ?? t;

        if (underlying == typeof(float) || underlying == typeof(double)) return "number";
        if (underlying == typeof(int) || underlying == typeof(long) || underlying == typeof(uint)) return "integer";
        if (underlying == typeof(bool)) return "boolean";
        if (underlying == typeof(string)) return "string";
        if (underlying == typeof(Guid)) return "guid";
        if (underlying == typeof(Vec3)) return "vec3";
        if (underlying.IsEnum)
            return "enum(" + string.Join("|", Enum.GetNames(underlying)) + ")";
        if (underlying.IsArray)
        {
            var elem = underlying.GetElementType()!;
            return "array<" + DescribeType(elem, verb, paramName) + ">";
        }
        // 显式注册的参数 DTO（[CliCommandObject]）→ schema "object"；字段形态
        // 由类型的 JSON 契约与命令 description/example 承载（返回值同惯例）。
        if (underlying.IsClass && underlying.IsDefined(typeof(CliCommandObjectAttribute), false))
            return "object";
        throw new CliFrameworkException(
            $"命令 {verb}: 参数 {paramName} 的类型 {t.Name} 没有 schema 规则。" +
            "请在 CliCommandRegistry.DescribeType 与绑定逻辑中登记该类型，或在同一次提交中改用受支持的类型。");
    }

    private static string DescribeReturnType(MethodInfo method, string verb)
    {
        var t = method.ReturnType;
        if (t == typeof(void) || t == typeof(Task)) return "null";
        if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(Task<>))
            return LooseTypeName(t.GetGenericArguments()[0]);
        return LooseTypeName(t);
    }

    /// <summary>
    /// Return types are serialized with standard System.Text.Json, so any
    /// serializable DTO is fine — only primitives get a precise schema name,
    /// everything else is reported as "object" (its shape lives in the
    /// description/example, which is where LLM callers look anyway).
    /// </summary>
    private static string LooseTypeName(Type t)
    {
        var underlying = Nullable.GetUnderlyingType(t) ?? t;
        if (underlying == typeof(float) || underlying == typeof(double)) return "number";
        if (underlying == typeof(int) || underlying == typeof(long) || underlying == typeof(uint)) return "integer";
        if (underlying == typeof(bool)) return "boolean";
        if (underlying == typeof(string)) return "string";
        if (underlying.IsEnum) return "enum(" + string.Join("|", Enum.GetNames(underlying)) + ")";
        return "object";
    }

    public CliCommandDescriptor? Find(string verb) =>
        verb != null && _commands.TryGetValue(verb, out var cmd) ? cmd : null;

    // === schema ===

    /// <summary>
    /// Aggregated command table with a $version content hash. CI snapshots
    /// this; MCP and clients cache against the version.
    /// </summary>
    public object BuildSchema()
    {
        var commands = Commands.Select(c => new Dictionary<string, object>
        {
            ["verb"] = c.Verb,
            ["channel"] = c.Channel,
            ["description"] = c.Description,
            ["example"] = c.Example ?? "",
            ["params"] = c.Params.Select(p => new Dictionary<string, object>
            {
                ["name"] = p.Name,
                ["type"] = p.Type,
                ["required"] = p.Required,
            }).ToArray(),
            ["returns"] = c.ReturnType,
        }).ToArray();

        return new Dictionary<string, object>
        {
            ["$version"] = ComputeVersion(),
            ["protocol"] = "primal.cli.v1",
            ["commands"] = commands,
        };
    }

    /// <summary>Deterministic content hash (first 16 hex chars of SHA-256 over
    /// the canonical command listing). Changes whenever any verb, description,
    /// parameter or example changes — CI gate + cache invalidation key.</summary>
    public string ComputeVersion()
    {
        var sb = new StringBuilder();
        foreach (var c in Commands)
        {
            sb.Append(c.Verb).Append('\u001f')
              .Append(c.Channel).Append('\u001f')
              .Append(c.Description).Append('\u001f')
              .Append(c.Example ?? "").Append('\u001f')
              .Append(c.ReturnType).Append('\u001f');
            foreach (var p in c.Params)
                sb.Append(p.Name).Append(':').Append(p.Type).Append(':').Append(p.Required ? '1' : '0').Append('\u001e');
            sb.Append('\u001d');
        }
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString()));
        return Convert.ToHexString(hash, 0, 8).ToLowerInvariant();
    }

    // === invocation ===

    /// <summary>
    /// Bind params, invoke the handler, package the response. Never throws —
    /// every failure becomes a structured CliResponse.
    /// </summary>
    public async Task<CliResponse> InvokeAsync(CliRequest request)
    {
        var response = new CliResponse { Id = request.Id };
        var exec = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            if (string.IsNullOrWhiteSpace(request.Verb))
                throw new CliError(CliErrorCodes.BadRequest, "缺少 verb 字段");

            var cmd = Find(request.Verb);
            if (cmd == null)
                throw new CliError(CliErrorCodes.BadVerb,
                    $"未知命令 '{request.Verb}'",
                    "调用 editor.schema 查看全部命令与参数");

            var args = BindParameters(cmd, request.Params);
            var result = cmd.Method.Invoke(cmd.Target, args);

            if (result is Task task)
            {
                await task.ConfigureAwait(false);
                result = task.GetType().IsGenericType
                    ? task.GetType().GetProperty("Result")!.GetValue(task)
                    : null;
            }

            response.Ok = true;
            response.Data = result == null
                ? null
                : JsonSerializer.SerializeToElement(result, JsonOptions);
        }
        catch (CliError err)
        {
            response.Ok = false;
            response.Error = new CliErrorPayload { Code = err.Code, Message = err.Message, Hint = err.Hint };
        }
        catch (System.Reflection.TargetInvocationException tie) when (tie.InnerException is CliError err)
        {
            response.Ok = false;
            response.Error = new CliErrorPayload { Code = err.Code, Message = err.Message, Hint = err.Hint };
        }
        catch (System.Reflection.TargetInvocationException tie) when (tie.InnerException is JsonException je)
        {
            response.Ok = false;
            response.Error = new CliErrorPayload
            {
                Code = CliErrorCodes.InvalidParams,
                Message = $"参数 JSON 无效: {je.Message}",
                Hint = "对照 editor.schema 中该命令的 params 示例修正",
            };
        }
        catch (Exception ex)
        {
            response.Ok = false;
            response.Error = new CliErrorPayload
            {
                Code = CliErrorCodes.Internal,
                Message = $"{ex.GetType().Name}: {ex.Message}",
                Hint = "查看编辑器日志定位；若可复现请连同 verb + params 上报",
            };
        }
        finally
        {
            exec.Stop();
            // QueueMs is filled in by the transport (server) which knows when
            // the request arrived; the registry only owns the exec time.
            response.Meta = new CliResponseMeta { QueueMs = 0, ExecMs = Math.Round(exec.Elapsed.TotalMilliseconds, 2) };
        }
        return response;
    }

    private object?[] BindParameters(CliCommandDescriptor cmd, JsonElement raw)
    {
        var parameters = cmd.Method.GetParameters();
        var args = new object?[parameters.Length];

        var byName = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (raw.ValueKind == JsonValueKind.Object)
        {
            foreach (var prop in raw.EnumerateObject())
                byName[prop.Name] = prop.Value;
        }
        else if (raw.ValueKind is not (JsonValueKind.Undefined or JsonValueKind.Null))
        {
            throw CliError.InvalidParams(
                $"命令 {cmd.Verb} 的 params 必须是 JSON 对象",
                $"示例: {cmd.Example ?? "(见 editor.schema)"}");
        }

        for (int i = 0; i < parameters.Length; i++)
        {
            var p = parameters[i];
            if (p.ParameterType == typeof(CancellationToken)) { args[i] = default(CancellationToken); continue; }

            if (!byName.TryGetValue(p.Name!, out var value))
            {
                if (p.HasDefaultValue) { args[i] = p.DefaultValue; continue; }
                throw CliError.InvalidParams(
                    $"命令 {cmd.Verb} 缺少必填参数 '{p.Name}' ({DescribeType(p.ParameterType, cmd.Verb, p.Name!)})",
                    $"完整参数示例: {cmd.Example ?? "(见 editor.schema)"}");
            }

            try
            {
                args[i] = JsonSerializer.Deserialize(value, p.ParameterType, JsonOptions);
            }
            catch (JsonException je)
            {
                throw CliError.InvalidParams(
                    $"命令 {cmd.Verb} 参数 '{p.Name}' 绑定失败 ({DescribeType(p.ParameterType, cmd.Verb, p.Name!)}): {je.Message}",
                    $"参数示例: {cmd.Example ?? "(见 editor.schema)"}");
            }
        }
        return args;
    }
}
