using System;
using System.Collections.Generic;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Snapshot of a launched process. Tests inject a fake
/// implementation; production code uses
/// <see cref="SystemProcessRunner"/>.
/// </summary>
public interface IProcessRunner
{
    /// <summary>
    /// Launch the supplied command and return a handle the caller can
    /// use to stream output and stop the process. The handle owns the
    /// underlying <see cref="System.Diagnostics.Process"/> and must be
    /// disposed by the caller (typically by the UI).
    /// </summary>
    IProcessHandle Start(BuildCommand command, Action<string> onOutput);
}

public interface IProcessHandle : IDisposable
{
    bool HasExited { get; }
    int? ExitCode { get; }
    void Kill();
    /// <summary>Subscribe to the process exit. Returns false if the process is already gone.</summary>
    bool SubscribeExit(Action<int> onExit);
}
