using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// PfxHash（C#）与引擎 PfxTypes.h::hash_name（C++ Murmur3-32, seed 0x9E3779B9）的
/// 跨语言一致性门禁。fixture 值来自引擎侧真实输出（TestParticleFX 调试探针 2026-09-13）。
/// </summary>
public class PfxHashTests
{
    [Theory]
    [InlineData("Nope", 1403263697u)]
    [InlineData("SpawnRate", 1082593107u)]
    [InlineData("SpawnBurst", 1749272776u)]
    [InlineData("ShapeLocation", 2074921921u)]
    [InlineData("GravityForce", 54510659u)]
    [InlineData("KillAge", 3686557556u)]
    public void HashName_MatchesEngineMurmur3(string name, uint expected)
    {
        Assert.Equal(expected, PfxHash.HashName(name));
    }
}
