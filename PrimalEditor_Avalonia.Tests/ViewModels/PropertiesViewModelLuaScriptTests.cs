using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// scripting S1 收官：Inspector Lua 参数编辑段的 VM 契约——行集来自组件
/// Parameters、添加/删除是编辑缓冲、应用 = 写回字典（跳过非法键）+ Undo +
/// 重建回调 + 标脏。与 script.set-param CLI 同语义（文档模型唯一事实源）。
/// </summary>
public sealed class PropertiesViewModelLuaScriptTests : IDisposable
{
    private readonly PropertiesViewModel _vm = new();
    private readonly GameEntity _entity = new("spinner");
    private readonly List<string> _resynced = new();
    private bool _dirty;
    private readonly UndoRedo _undoRedo = new();

    public PropertiesViewModelLuaScriptTests()
    {
        _entity.Components.Add(new Transform());
        _entity.Components.Add(new LuaScriptComponent());
        PropertiesViewModel.ScriptResyncCallback = _ => _resynced.Add("resync");
        _vm.MarkProjectDirtyCallback = () => _dirty = true;
        _vm.UndoRedoQuery = () => _undoRedo;
    }

    public void Dispose() => PropertiesViewModel.ScriptResyncCallback = null;

    private LuaScriptComponent Script => _entity.GetComponent<LuaScriptComponent>()!;

    private void Select() => _vm.Entities = new[] { _entity };

    [Fact]
    public void SelectingEntityWithScript_BuildsSortedParamRows()
    {
        Script.Parameters["speed"] = "0.8";
        Script.Parameters["axis"] = "x";
        Select();

        Assert.True(_vm.HasLuaScriptComponent);
        var rows = _vm.LuaParamRows!;
        Assert.Equal(new[] { "axis", "speed" }, rows.Select(r => r.Name).ToArray());
        Assert.Equal("0.8", rows.First(r => r.Name == "speed").Value);
    }

    [Fact]
    public void EntityWithoutScript_HidesSection()
    {
        var plain = new GameEntity("plain");
        plain.Components.Add(new Transform());
        _vm.Entities = new[] { plain };

        Assert.False(_vm.HasLuaScriptComponent);
        Assert.Null(_vm.LuaParamRows);
    }

    [Fact]
    public void AddParam_RejectsSpaceAndDuplicateKeys()
    {
        Script.Parameters["speed"] = "0.8";
        Select();
        var rows = _vm.LuaParamRows!;
        var count = rows.Count;

        _vm.LuaNewParamName = "bad key";
        _vm.LuaAddParamCommand.Execute(null);
        _vm.LuaNewParamName = "speed"; // 重复
        _vm.LuaAddParamCommand.Execute(null);
        Assert.Equal(count, rows.Count);

        _vm.LuaNewParamName = "scale";
        _vm.LuaNewParamValue = "2";
        _vm.LuaAddParamCommand.Execute(null);
        Assert.Equal(count + 1, rows.Count);
        Assert.Equal("scale", rows[^1].Name);
        Assert.Equal("2", rows[^1].Value);
        // 输入缓冲清空
        Assert.Equal(string.Empty, _vm.LuaNewParamName);
    }

    [Fact]
    public void ApplyParams_WritesBack_SkipsInvalidKeys_AndFiresCallbacks()
    {
        Script.Parameters["speed"] = "0.8";
        Script.Parameters["stale"] = "1";
        Select();
        var rows = _vm.LuaParamRows!;
        rows.First(r => r.Name == "speed").Value = "1.5";
        rows.Remove(rows.First(r => r.Name == "stale"));
        _vm.LuaAddParamCommand.Execute(null); // 空名 → 无效，行集不变

        _vm.LuaApplyParamsCommand.Execute(null);

        Assert.Equal(1, Script.Parameters.Count);
        Assert.Equal("1.5", Script.Parameters["speed"]);
        Assert.Equal(1, _resynced.Count);
        Assert.True(_dirty);
    }

    [Fact]
    public void ApplyParams_WithInvalidRow_SkipsOnlyThatRow()
    {
        Select();
        var rows = _vm.LuaParamRows!;
        rows.Add(new ScriptParamRow("ok", "1"));
        rows.Add(new ScriptParamRow("bad key", "2")); // 应用时被拒

        _vm.LuaApplyParamsCommand.Execute(null);

        Assert.Single(Script.Parameters);
        Assert.Equal("1", Script.Parameters["ok"]);
    }

    [Fact]
    public void ApplyParams_NoChange_DoesNotResyncOrDirty()
    {
        Script.Parameters["speed"] = "0.8";
        Select();

        _vm.LuaApplyParamsCommand.Execute(null);

        Assert.Empty(_resynced);
        Assert.False(_dirty);
    }

    [Fact]
    public void ApplyParams_IsUndoable()
    {
        Script.Parameters["speed"] = "0.8";
        Select();
        _vm.LuaParamRows!.First(r => r.Name == "speed").Value = "1.5";

        _vm.LuaApplyParamsCommand.Execute(null);
        Assert.Equal("1.5", Script.Parameters["speed"]);

        _undoRedo.Undo();
        Assert.Equal("0.8", Script.Parameters["speed"]);
        _undoRedo.Redo();
        Assert.Equal("1.5", Script.Parameters["speed"]);
        // undo/redo 各触发一次重建（DelegateAction 内的 ScriptResyncCallback）
        Assert.Equal(3, _resynced.Count);
    }

    [Fact]
    public void AssetName_FallsBackToScriptNameThenPlaceholder()
    {
        Script.ScriptName = "rotate.lua";
        Select();
        Assert.Equal("rotate.lua", _vm.LuaScriptAssetName);

        Script.ScriptName = "";
        _vm.Entities = new[] { _entity };
        Assert.Equal("(未绑定)", _vm.LuaScriptAssetName);
    }
}
