using System;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Build;

public sealed class BuildServiceTests
{
    [Fact]
    public void Start_RejectsEmptyConfig()
    {
        BuildService.Override(new FakeResolver(true, "DebugEditor"), new FakeRunner());
        var ok = BuildService.Start("");
        Assert.False(ok);
    }

    [Fact]
    public void Start_RejectsUnsupportedPlatform()
    {
        BuildService.Override(new FakeResolver(false, "DebugEditor"), new FakeRunner());
        Assert.False(BuildService.Start("DebugEditor"));
    }

    [Fact]
    public void Start_DelegatesCommandToRunner()
    {
        var resolver = new FakeResolver(true, "DebugEditor");
        var runner = new FakeRunner();
        BuildService.Override(resolver, runner);
        Assert.True(BuildService.Start("DebugEditor"));
        Assert.NotNull(runner.LastCommand);
        Assert.Equal("DebugEditor", runner.LastCommand!.Arguments[2]);
    }

    [Fact]
    public void Stop_KillsRunningProcess()
    {
        var resolver = new FakeResolver(true, "DebugEditor");
        var runner = new FakeRunner();
        BuildService.Override(resolver, runner);
        BuildService.Start("DebugEditor");
        Assert.True(BuildService.IsRunning || runner.LastHandle != null);
        BuildService.Stop();
        Assert.True(runner.LastHandle!.Killed);
    }

    [Fact]
    public void CompletedEvent_FiresOnExit()
    {
        var resolver = new FakeResolver(true, "DebugEditor");
        var runner = new FakeRunner { ExitCode = 0 };
        BuildService.Override(resolver, runner);
        int? reported = null;
        bool? ok = null;
        BuildService.OnCompleted += (code, success) => { reported = code; ok = success; };
        BuildService.Start("DebugEditor");
        runner.SimulateExit();
        Assert.Equal(0, reported);
        Assert.True(ok);
    }

    private sealed class FakeResolver : IBuildCommandResolver
    {
        private readonly bool _ok;
        private readonly string _config;
        public FakeResolver(bool ok, string config) { _ok = ok; _config = config; }
        public bool TryResolve(BuildAction action, string config, out BuildCommand command)
        {
            if (!_ok || config != _config) { command = default!; return false; }
            command = new BuildCommand("/bin/zsh", new[] { "build_engine.sh", action.ToString().ToLowerInvariant(), config }, ".");
            return true;
        }
    }

    private sealed class FakeRunner : IProcessRunner
    {
        public BuildCommand? LastCommand { get; private set; }
        public FakeHandle? LastHandle { get; private set; }
        public int ExitCode { get; set; } = 0;
        public IProcessHandle Start(BuildCommand command, Action<string> onOutput)
        {
            LastCommand = command;
            LastHandle = new FakeHandle(this);
            return LastHandle;
        }
        public void SimulateExit()
        {
            LastHandle?.Simulate(ExitCode);
        }
    }

    private sealed class FakeHandle : IProcessHandle
    {
        private readonly FakeRunner _owner;
        private Action<int>? _exit;
        public bool Killed { get; private set; }
        public FakeHandle(FakeRunner owner) { _owner = owner; }
        public bool HasExited { get; private set; }
        public int? ExitCode => _owner.ExitCode;
        public bool SubscribeExit(Action<int> onExit) { _exit = onExit; return true; }
        public void Kill() { Killed = true; Simulate(_owner.ExitCode); }
        public void Dispose() { }
        public void Simulate(int code)
        {
            if (HasExited) return;
            HasExited = true;
            _exit?.Invoke(code);
        }
    }
}
