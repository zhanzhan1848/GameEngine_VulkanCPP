using System;
using System.Diagnostics;

namespace PrimalEditor_Avalonia.Common;

public sealed class SystemProcessRunner : IProcessRunner
{
    public IProcessHandle Start(BuildCommand command, Action<string> onOutput)
    {
        var psi = new ProcessStartInfo
        {
            FileName = command.Executable,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        if (!string.IsNullOrEmpty(command.WorkingDirectory))
            psi.WorkingDirectory = command.WorkingDirectory;
        foreach (var a in command.Arguments) psi.ArgumentList.Add(a);

        var proc = Process.Start(psi);
        if (proc == null) throw new InvalidOperationException("Process.Start returned null");
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) onOutput?.Invoke(e.Data); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) onOutput?.Invoke(e.Data); };
        proc.EnableRaisingEvents = true;
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
        return new ProcessHandle(proc);
    }

    private sealed class ProcessHandle : IProcessHandle
    {
        private Process? _proc;
        public ProcessHandle(Process p) { _proc = p; }
        public bool HasExited => _proc?.HasExited ?? true;
        public int? ExitCode => _proc?.ExitCode;
        public void Kill()
        {
            try { _proc?.Kill(true); }
            catch (Exception ex) { Logger.Warning($"SystemProcessRunner: Kill failed — {ex.Message}"); }
        }
        public bool SubscribeExit(Action<int> onExit)
        {
            if (_proc == null) return false;
            _proc.Exited += (_, _) => onExit?.Invoke(_proc.ExitCode);
            return true;
        }
        public void Dispose()
        {
            if (_proc == null) return;
            try { if (!_proc.HasExited) _proc.Kill(true); } catch { }
            _proc.Dispose();
            _proc = null;
        }
    }
}
