using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Build;

public sealed class FakeProcessRunner : IProcessRunner
{
    public BuildCommand? LastCommand { get; private set; }
    public Action<string>? OutputToEmit { get; set; }
    public int ExitCode { get; set; } = 0;
    public bool ThrowOnStart { get; set; }

    public IProcessHandle Start(BuildCommand command, Action<string> onOutput)
    {
        LastCommand = command;
        if (ThrowOnStart) throw new InvalidOperationException("simulated start failure");
        OutputToEmit = onOutput;
        return new FakeHandle(this);
    }

    public sealed class FakeHandle : IProcessHandle
    {
        private readonly FakeProcessRunner _owner;
        public FakeHandle(FakeProcessRunner o) { _owner = o; }
        public bool HasExited { get; private set; }
        public int? ExitCode => _owner.ExitCode;
        public void Kill() { HasExited = true; }
        public bool SubscribeExit(Action<int> onExit)
        {
            HasExited = true;
            onExit?.Invoke(_owner.ExitCode);
            return true;
        }
        public void Dispose() { }
    }
}

public sealed class BuildErrorParserTests
{
    [Fact]
    public void RecognisesGccStyleDiagnostic()
    {
        var hit = BuildErrorParser.Parse("src/foo.cpp:42:7: error: use of undeclared identifier 'x'");
        Assert.NotNull(hit);
        Assert.Equal("src/foo.cpp", hit!.Value.File);
        Assert.Equal(42, hit.Value.Line);
        Assert.Equal(7, hit.Value.Column);
    }

    [Fact]
    public void RecognisesClangStyleDiagnostic()
    {
        var hit = BuildErrorParser.Parse("Engine/Bar.h:120:5: warning: unused variable 'tmp'");
        Assert.NotNull(hit);
        Assert.Equal("Engine/Bar.h", hit!.Value.File);
        Assert.Equal(120, hit.Value.Line);
        Assert.Equal(5, hit.Value.Column);
    }

    [Fact]
    public void ReturnsNullForNonDiagnosticLine()
    {
        Assert.Null(BuildErrorParser.Parse("Building CXX object foo.cpp.o"));
        Assert.Null(BuildErrorParser.Parse("Linking CXX executable mygame"));
        Assert.Null(BuildErrorParser.Parse(""));
    }

    [Fact]
    public void AcceptsWindowsDriveLetterInPath()
    {
        var hit = BuildErrorParser.Parse("C:\\src\\foo.cpp:12:3: error: bad");
        Assert.NotNull(hit);
        Assert.Equal(@"C:\src\foo.cpp", hit!.Value.File);
        Assert.Equal(12, hit.Value.Line);
        Assert.Equal(3, hit.Value.Column);
    }
}
