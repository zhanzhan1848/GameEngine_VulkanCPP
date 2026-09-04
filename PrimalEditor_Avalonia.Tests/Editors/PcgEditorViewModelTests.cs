using System;
using System.IO;
using PrimalEditor_Avalonia.Editors.Pcg;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

// A6 PCG 编辑器回归：无引擎加载环境下的安全降级（NativeEngineBridge 未加载
// 时所有 PCG 绑定 no-op，Load/Save/Execute 不得抛异常）。
public sealed class PcgEditorViewModelTests : IDisposable
{
    private readonly string _path;

    public PcgEditorViewModelTests()
    {
        _path = Path.Combine(Path.GetTempPath(), $"a6_pcg_{Guid.NewGuid():N}.pcg");
    }

    public void Dispose()
    {
        if (File.Exists(_path)) File.Delete(_path);
    }

    [Fact]
    public void SetAsset_MissingFile_ShowsErrorWithoutThrowing()
    {
        var vm = new PcgEditorViewModel();
        vm.SetAsset(new PrimalEditor_Avalonia.Content.AssetInfo { FullPath = _path });
        Assert.False(vm.IsDirty);
        Assert.Contains("missing", vm.StatusLine, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SetAsset_ValidJson_DoesNotThrow_RegardlessOfEngine()
    {
        // 图 JSON 与 PCGSerializer 格式一致；引擎加载与否决定画布是否重建，
        // 但任何情况下不得抛异常（内容浏览器双击路径）。
        File.WriteAllText(_path,
            "{\"nodes\":[{\"type\":\"NoiseField\",\"params\":{\"frequency\":0.05}}]," +
            "\"connections\":[]}");
        var vm = new PcgEditorViewModel();
        vm.SetAsset(new PrimalEditor_Avalonia.Content.AssetInfo { FullPath = _path });
        Assert.NotEmpty(vm.StatusLine);
    }

    [Fact]
    public void SaveWithoutEngine_IsGracefulNoOp()
    {
        File.WriteAllText(_path, "{\"nodes\":[],\"connections\":[]}");
        var vm = new PcgEditorViewModel();
        vm.SetAsset(new PrimalEditor_Avalonia.Content.AssetInfo { FullPath = _path });
        vm.Save(); // 引擎未加载时 SerializeGraph 返回 null → 状态行报错，不抛
        Assert.NotEmpty(vm.StatusLine);
    }

    [Fact]
    public void ExecuteWithoutEngine_IsGracefulNoOp()
    {
        var vm = new PcgEditorViewModel();
        vm.Execute();
        Assert.False(vm.IsDirty);
    }
}
