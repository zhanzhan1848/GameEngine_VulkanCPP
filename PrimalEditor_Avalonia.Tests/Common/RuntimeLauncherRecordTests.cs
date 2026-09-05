using System;
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Common;

/// <summary>
/// Run 录制（Part B）的参数拼装与目录约定。语义契约对齐
/// Tools/PrimalPlayer/main.cpp 的 argparse：--record 目录、--record-frames
/// 捕获帧上限（0 = 跟随窗口关闭）、--record-every 步长（1 时省略）、
/// --record-metrics 开关。player 侧一旦改参数顺序/语义，这里必须同步。
/// </summary>
public sealed class RuntimeLauncherRecordTests
{
    [Fact]
    public void AppendRecordArguments_Defaults_PassDirFramesAndMetrics()
    {
        var args = new List<string> { "--scene", "/tmp/x.primalrun" };
        RuntimeLauncher.AppendRecordArguments(args, "/tmp/rec",
            new RuntimeLauncher.PlayerRecordOptions(Enabled: true, Frames: 300, Every: 1, Metrics: true));

        Assert.Equal(new[]
        {
            "--scene", "/tmp/x.primalrun",
            "--record", "/tmp/rec",
            "--record-frames", "300",
            // player 的行式解析器要求 --record-metrics 带显式路径（无值即 unknown arg）。
            "--record-metrics", Path.Combine("/tmp/rec", "metrics.csv"),
        }, args);
    }

    [Fact]
    public void AppendRecordArguments_StrideOne_OmitsEveryFlag()
    {
        var args = new List<string>();
        RuntimeLauncher.AppendRecordArguments(args, "/tmp/rec",
            new RuntimeLauncher.PlayerRecordOptions(true, 60, Every: 1, Metrics: false));

        Assert.DoesNotContain("--record-every", args);
        Assert.DoesNotContain("--record-metrics", args);
    }

    [Fact]
    public void AppendRecordArguments_StrideAboveOne_IncludesEveryFlag()
    {
        var args = new List<string>();
        RuntimeLauncher.AppendRecordArguments(args, "/tmp/rec",
            new RuntimeLauncher.PlayerRecordOptions(true, 0, Every: 4, Metrics: false));

        var everyIndex = args.IndexOf("--record-every");
        Assert.True(everyIndex >= 0 && everyIndex + 1 < args.Count);
        Assert.Equal("4", args[everyIndex + 1]);
    }

    [Fact]
    public void AppendRecordArguments_NegativeFrames_ClampedToZero()
    {
        // 0 = 跟随窗口关闭；负数在托管侧就钳掉，不把脏值传给 player。
        var args = new List<string>();
        RuntimeLauncher.AppendRecordArguments(args, "/tmp/rec",
            new RuntimeLauncher.PlayerRecordOptions(true, Frames: -5, Every: 1, Metrics: false));

        var framesIndex = args.IndexOf("--record-frames");
        Assert.Equal("0", args[framesIndex + 1]);
    }

    [Fact]
    public void ResolveRecordDirectory_SanitizesSceneNameAndNestsUnderProject()
    {
        var dir = RuntimeLauncher.ResolveRecordDirectory("/proj", @"a<b>:c");

        Assert.StartsWith(Path.Combine("/proj", "Captures"), dir);
        Assert.DoesNotContain("<", dir);
        Assert.DoesNotContain(":", dir.Substring(dir.IndexOf("Captures", StringComparison.Ordinal)));
        Assert.Contains("a_b__c", dir);
    }

    [Theory]
    [InlineData("Metal", "Debug", "Darwin/Debug")]
    [InlineData("metal", "Release", "Darwin/Release")]
    [InlineData("Vulkan", "Debug", "Darwin/DebugVulkan")]
    [InlineData("Vulkan", "Release", "Darwin/DebugVulkan")]
    public void ResolvePlayerDir_MapsApiAndConfigToBuildTree(string api, string config, string expected)
    {
        Assert.Equal(expected, RuntimeLauncher.ResolvePlayerDir(api, config));
    }
}
