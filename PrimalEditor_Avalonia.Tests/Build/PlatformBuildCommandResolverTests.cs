using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Build;

public sealed class PlatformBuildCommandResolverTests
{
    private sealed class FixedPlatform : IPlatformProbe
    {
        public FixedPlatform(HostPlatform p) { Current = p; }
        public HostPlatform Current { get; }
    }

    [Fact]
    public void MacOS_UsesZshAndEditorBuildScript()
    {
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal-build-test-mac");
        var resolver = new PlatformBuildCommandResolver(new FixedPlatform(HostPlatform.MacOS), dir);
        var ok = resolver.TryResolve(BuildAction.Build, "DebugEditor", out var cmd);
        Assert.True(ok);
        Assert.Equal("/bin/zsh", cmd.Executable);
        Assert.Equal(3, cmd.Arguments.Count);
        Assert.Equal(System.IO.Path.Combine(dir, "build_engine.sh"), cmd.Arguments[0]);
        Assert.Equal("build", cmd.Arguments[1]);
        Assert.Equal("DebugEditor", cmd.Arguments[2]);
        Assert.Equal(dir, cmd.WorkingDirectory);
    }

    [Fact]
    public void Linux_UsesBashAndEditorBuildScript()
    {
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal-build-test-linux");
        var resolver = new PlatformBuildCommandResolver(new FixedPlatform(HostPlatform.Linux), dir);
        var ok = resolver.TryResolve(BuildAction.Rebuild, "ReleaseEditor", out var cmd);
        Assert.True(ok);
        Assert.Equal("/bin/bash", cmd.Executable);
        Assert.Equal("rebuild", cmd.Arguments[1]);
        Assert.Equal("ReleaseEditor", cmd.Arguments[2]);
    }

    [Fact]
    public void Windows_ReturnsFalseSoEditorSurfacesError()
    {
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal-build-test-win");
        var resolver = new PlatformBuildCommandResolver(new FixedPlatform(HostPlatform.Windows), dir);
        var ok = resolver.TryResolve(BuildAction.Build, "DebugEditor", out _);
        Assert.False(ok);
    }

    [Fact]
    public void EmptyConfig_IsRejected()
    {
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal-build-test-empty");
        var resolver = new PlatformBuildCommandResolver(new FixedPlatform(HostPlatform.MacOS), dir);
        var ok = resolver.TryResolve(BuildAction.Build, "", out _);
        Assert.False(ok);
    }

    [Fact]
    public void RunAction_PassesRunVerb()
    {
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal-build-test-run");
        var resolver = new PlatformBuildCommandResolver(new FixedPlatform(HostPlatform.MacOS), dir);
        var ok = resolver.TryResolve(BuildAction.Run, "DebugEditor", out var cmd);
        Assert.True(ok);
        Assert.Equal("run", cmd.Arguments[1]);
    }
}
