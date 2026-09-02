using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text.Json;

namespace Primal.CliFramework;

/// <summary>Thrown at registration time when a command signature cannot be
/// described by the schema (fail-fast, per the CLI plan §4.3 — otherwise
/// drift returns silently).</summary>
public sealed class CliFrameworkException : Exception
{
    public CliFrameworkException(string message) : base(message) { }
}

/// <summary>One registered command: attribute metadata + reflected handler.</summary>
public sealed class CliCommandDescriptor
{
    public required string Verb { get; init; }
    public required string Description { get; init; }
    public string? Example { get; init; }
    public required string Channel { get; init; }
    public required CliParamDescriptor[] Params { get; init; }
    public required string ReturnType { get; init; }

    internal MethodInfo Method { get; init; } = null!;
    internal object? Target { get; init; }
}

public sealed class CliParamDescriptor
{
    public required string Name { get; init; }
    /// <summary>Schema type name: number / integer / boolean / string / vec3 /
    /// array&lt;T&gt; / enum(A|B|C) / guid.</summary>
    public required string Type { get; init; }
    public required bool Required { get; init; }
}
