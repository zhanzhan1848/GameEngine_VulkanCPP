using System;

namespace Primal.CliFramework;

/// <summary>
/// Marks a method as an invocable CLI/Agent command. The verb must be unique
/// across the registry and use dotted namespace form ("editor.ping").
/// The registry is the single source of truth: command discovery (editor.schema),
/// JSON schema generation and MCP tool definitions are all derived from this
/// attribute plus the method signature — never hand-written a second time.
/// </summary>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
public sealed class CliCommandAttribute : Attribute
{
    /// <summary>Dotted verb, e.g. "editor.ping" or "entity.set-transform".</summary>
    public string Verb { get; }

    /// <summary>
    /// What the command does and — critically for LLM callers — units,
    /// coordinate system, preconditions, failure semantics and side effects.
    /// At least one <see cref="Example"/> per verb is the documentation bar.
    /// </summary>
    public string Description { get; set; } = "";

    /// <summary>A complete params JSON example, e.g. {"entityGuid":"..."}.</summary>
    public string? Example { get; set; }

    /// <summary>
    /// Execution channel: "editor" (routed to the running editor over the
    /// command socket) or "offline" (one-shot subprocess). The aggregated
    /// schema labels every command so clients can route without knowing
    /// the process topology.
    /// </summary>
    public string Channel { get; set; } = "editor";

    public CliCommandAttribute(string verb)
    {
        if (string.IsNullOrWhiteSpace(verb))
            throw new ArgumentException("Verb must not be empty", nameof(verb));
        Verb = verb;
    }
}
