using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Common;

/// <summary>
/// Run 模式三选一（§Run）的纯函数契约：模式解析与固定分辨率参数拼装。
/// 语义契约对齐 Tools/PrimalPlayer/main.cpp 的 argparse（--width/--height/
/// --fixed）；player 侧一旦改参数语义，这里必须同步。
/// </summary>
public sealed class RuntimeLauncherRunModeTests
{
    [Theory]
    [InlineData(0, RuntimeLauncher.RunMode.PlayerWindow)]
    [InlineData(1, RuntimeLauncher.RunMode.FixedResolution)]
    [InlineData(2, RuntimeLauncher.RunMode.RenderSurface)]
    [InlineData(7, RuntimeLauncher.RunMode.RenderSurface)]  // 越界收敛（VM setter 本应钳位）
    [InlineData(-1, RuntimeLauncher.RunMode.PlayerWindow)]
    public void ResolveMode_MapsIndexToEnum(int index, RuntimeLauncher.RunMode expected)
    {
        Assert.Equal(expected, RuntimeLauncher.ResolveMode(index));
    }

    [Fact]
    public void AppendResolutionArguments_PassesWidthHeightAndFixedFlag()
    {
        var args = new List<string> { "--scene", "/tmp/x.primalrun" };
        RuntimeLauncher.AppendResolutionArguments(args, 1920, 1080);

        Assert.Equal(new[]
        {
            "--scene", "/tmp/x.primalrun",
            "--width", "1920",
            "--height", "1080",
            "--fixed",
        }, args);
    }

    [Theory]
    [InlineData(0, 16)]
    [InlineData(99999, 8192)]
    public void AppendResolutionArguments_ClampsToPlayerSafeRange(int input, int expected)
    {
        var args = new List<string>();
        RuntimeLauncher.AppendResolutionArguments(args, input, input);

        Assert.Equal(expected.ToString(), args[args.IndexOf("--width") + 1]);
        Assert.Equal(expected.ToString(), args[args.IndexOf("--height") + 1]);
    }
}
