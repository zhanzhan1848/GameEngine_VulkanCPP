using System;

namespace Primal.CliFramework;

/// <summary>
/// Error codes used in CliResponse.error.code. The client maps them onto the
/// documented process exit codes (0/2/3/4/5) — keep this list in sync with
/// CliExitCodes.
/// </summary>
public static class CliErrorCodes
{
    public const string BadRequest = "BAD_REQUEST";       // unparseable frame
    public const string BadVerb = "BAD_VERB";             // unknown command
    public const string InvalidParams = "INVALID_PARAMS"; // binding/validation failure
    public const string NotFound = "NOT_FOUND";           // referenced resource missing
    public const string NotReady = "NOT_READY";           // command needs state that is not there yet (no scene, no viewport)
    public const string Internal = "INTERNAL";            // unexpected host failure
    public const string EngineError = "ENGINE_ERROR";     // native engine rejected the operation
    public const string NotConnected = "NOT_CONNECTED";   // client-side: editor not running / socket gone
}

/// <summary>Process exit codes for the primal CLI client.</summary>
public static class CliExitCodes
{
    public const int Success = 0;
    public const int UsageError = 2;        // invalid client arguments or INVALID_PARAMS/BAD_VERB/BAD_REQUEST
    public const int NotFound = 3;          // NOT_FOUND
    public const int EngineError = 4;       // INTERNAL / ENGINE_ERROR / NOT_READY and anything else
    public const int EditorNotRunning = 5;  // no live command socket
}

/// <summary>
/// Business error thrown by command handlers. The framework converts it into
/// a structured response; the message must stand alone, and <paramref name="hint"/>
/// should tell an LLM caller what to try next ("先用 scene.tree 查询有效 guid").
/// </summary>
public class CliError : Exception
{
    public string Code { get; }
    public string? Hint { get; }

    public CliError(string code, string message, string? hint = null) : base(message)
    {
        Code = code;
        Hint = hint;
    }

    public static CliError NotFound(string what, string hint) =>
        new(CliErrorCodes.NotFound, $"{what} 不存在", hint);

    public static CliError NotReady(string what, string hint) =>
        new(CliErrorCodes.NotReady, $"{what}", hint);

    public static CliError InvalidParams(string message, string? hint = null) =>
        new(CliErrorCodes.InvalidParams, message, hint);
}
