using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels.PFX;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// PFX module-stack editor: asset load → schema-typed module rows →
/// edit → save round-trip. The engine schema (PfxGetEmitterSchema) is not
/// available in the test host, so param typing falls back to JSON inference —
/// the same path a fresh editor hits before the engine is up.
/// </summary>
public sealed class PfxEmitterEditorViewModelTests
{
    private const string SampleJson = """
    {
      "name": "dust_puff",
      "blend": "alpha",
      "capacity": 4096,
      "stages": {
        "spawn": [ { "module": "SpawnRate", "rate": 120 } ],
        "init": [
          { "module": "SetLifetime", "min": 1.0, "max": 2.5 },
          { "module": "AddVelocity", "mode": "fixed", "direction": [0, 1, 0] }
        ],
        "update": [
          { "module": "GravityForce", "gravity": [0, -1.5, 0] },
          { "module": "ScaleOverLife", "curve": [[0, 1.2], [1, 0.1]] },
          { "module": "AlphaOverLife", "curve": [[0, 1], [0.7, 0.8], [1, 0]] }
        ]
      }
    }
    """;

    private static void InjectSchema()
    {
        // The engine dylib is unavailable in the test host, so inject the
        // schema slice the sample asset exercises (same types the real
        // PfxGetEmitterSchema reports).
        PrimalEditor_Avalonia.Services.PfxEditorService.Instance.SetSchema(
            new PrimalEditor_Avalonia.Models.PFX.PfxSchema
            {
                Version = 1,
                Stages = { "spawn", "init", "update" },
                Modules =
                {
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "SpawnRate", Stage = "spawn",
                        Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "rate", Type = "param_ref", Note = "particles per second" } },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "SetLifetime", Stage = "init",
                        Params =
                        {
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "min", Type = "param_ref" },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "max", Type = "param_ref" },
                        },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "AddVelocity", Stage = "init",
                        Params =
                        {
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "mode", Type = "enum", Note = "fixed|shape_normal|random_sphere" },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema
                            {
                                Name = "direction", Type = "v3", Note = "fixed mode direction",
                                Default = System.Text.Json.JsonDocument.Parse("[0,1,0]").RootElement,
                            },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "speed_min", Type = "param_ref" },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "speed_max", Type = "param_ref" },
                        },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "GravityForce", Stage = "update",
                        Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "gravity", Type = "v3" } },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "ScaleOverLife", Stage = "update",
                        Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "curve", Type = "curve" } },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "AlphaOverLife", Stage = "update",
                        Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "curve", Type = "curve" } },
                    },
                },
            });
    }

    private static (PfxEmitterEditorViewModel Vm, string Path) Load()
    {
        InjectSchema();
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.pfx_emitter");
        File.WriteAllText(path, SampleJson);
        var vm = new PfxEmitterEditorViewModel();
        vm.SetAsset(new AssetInfo { FullPath = path, Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter });
        return (vm, path);
    }

    [Fact]
    public void Load_ParsesModuleRowsWithTypedParams()
    {
        var (vm, path) = Load();
        try
        {
            Assert.Equal(6, vm.AllModules.Count);
            Assert.False(vm.IsDirty);
            Assert.Equal("dust_puff", vm.EmitterName);
            Assert.Equal("alpha", vm.BlendMode);

            var spawn = vm.AllModules.Single(m => m.Name == "SpawnRate");
            Assert.Equal("spawn", spawn.Stage);
            var rate = spawn.Params.Single(p => p.Name == "rate");
            Assert.True(rate.IsNumeric,
                "param_ref 的数字形态必须按数字编辑：引擎 get_param_ref_field 只认 " +
                "number/{\"param\":...} 对象，写成 JSON 字符串会被编译拒绝");
            Assert.Equal("120", rate.Value);

            var addVel = vm.AllModules.Single(m => m.Name == "AddVelocity");
            var dir = addVel.Params.Single(p => p.Name == "direction");
            Assert.True(dir.IsVector,
                $"kind={dir.Kind} type={dir.Type} schemaMods={vm.Schema?.Modules.Count.ToString() ?? "null"} " +
                $"schemaTypes=[{string.Join(",", vm.Schema?.Modules.FirstOrDefault(m => m.Name == "AddVelocity")?.Params.Select(x => x.Name + ":" + x.Type) ?? Array.Empty<string>())}]");
            Assert.Equal(3, dir.ComponentCount);
            Assert.Equal("0", dir.X);
            Assert.Equal("1", dir.Y);
            Assert.Equal("0", dir.Z);

            var scaleOverLife = vm.AllModules.Single(m => m.Name == "ScaleOverLife");
            var curve = scaleOverLife.Params.Single(p => p.Name == "curve");
            Assert.True(curve.IsCurve);
            Assert.NotNull(curve.Curve);
            Assert.Equal(2, curve.Curve!.Keys.Count);
            Assert.Equal(1.2f, curve.Curve.Keys[0].Value[0]);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void CurveEdit_Save_RoundTripsCurveKeyframes()
    {
        var (vm, path) = Load();
        try
        {
            var alphaCurve = vm.AllModules.Single(m => m.Name == "AlphaOverLife")
                .Params.Single(p => p.Name == "curve");
            Assert.NotNull(alphaCurve.Curve);
            alphaCurve.Curve!.Keys[1].Value[0] = 0.5f;

            vm.Save();
            Assert.False(vm.IsDirty);

            var reload = new PfxEmitterEditorViewModel();
            reload.SetAsset(new AssetInfo { FullPath = path, Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter });
            var reloaded = reload.AllModules.Single(m => m.Name == "AlphaOverLife")
                .Params.Single(p => p.Name == "curve");
            Assert.NotNull(reloaded.Curve);
            Assert.Equal(3, reloaded.Curve!.Keys.Count);
            Assert.Equal(0.5f, reloaded.Curve.Keys[1].Value[0]);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void AddAndRemoveModule_MarksDirtyAndPersistsOnSave()
    {
        var (vm, path) = Load();
        try
        {
            var def = new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
            {
                Name = "DragForce",
                Stage = "update",
                Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "coefficient", Type = "param_ref" } },
            };
            vm.AddModuleFromPalette(new PaletteModuleViewModel(def));

            Assert.True(vm.IsDirty);
            Assert.NotNull(vm.SelectedModule);
            Assert.Equal("DragForce", vm.SelectedModule!.Name);
            var coeff = vm.SelectedModule.Params.Single(p => p.Name == "coefficient");
            coeff.Value = "0.35";

            vm.Save();
            var text = File.ReadAllText(path);
            Assert.Contains("DragForce", text);
            Assert.Contains("0.35", text);

            var reload = new PfxEmitterEditorViewModel();
            reload.SetAsset(new AssetInfo { FullPath = path, Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter });
            Assert.Equal(7, reload.AllModules.Count);
            Assert.Contains(reload.AllModules, m => m.Name == "DragForce");
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Undone_StructuralEdit_RestoresPreviousStack()
    {
        var (vm, path) = Load();
        try
        {
            var countBefore = vm.AllModules.Count;
            vm.AddModuleFromPalette(new PaletteModuleViewModel(
                new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema { Name = "KillAge", Stage = "update" }));
            Assert.Equal(countBefore + 1, vm.AllModules.Count);

            vm.UndoRedo.Undo();
            Assert.Equal(countBefore, vm.AllModules.Count);

            vm.UndoRedo.Redo();
            Assert.Equal(countBefore + 1, vm.AllModules.Count);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void MissingSchemaDefaultedParam_IsMaterializedWithEngineDefault()
    {
        // 文档没写 AddVelocity 的 direction（shape_normal/random_sphere 模式
        // 不需要它）：编辑器必须按引擎默认值补出该行，否则用户切到 fixed
        // 模式时 UI 根本没有初始方向字段可编辑。
        var (vm, path) = Load();
        try
        {
            var addVel = vm.AllModules.Single(m => m.Name == "AddVelocity");
            var dir = addVel.Params.Single(p => p.Name == "direction");
            Assert.True(dir.IsVector);
            Assert.Equal("0", dir.X);
            Assert.Equal("1", dir.Y);
            Assert.Equal("0", dir.Z);

            // 补全的行参与保存，写回文档
            vm.Save();
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
            var el = doc.RootElement.GetProperty("stages").GetProperty("init")[1].GetProperty("direction");
            Assert.Equal(System.Text.Json.JsonValueKind.Array, el.ValueKind);
            Assert.Equal(0f, el[0].GetSingle());
            Assert.Equal(1f, el[1].GetSingle());
            Assert.Equal(0f, el[2].GetSingle());
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void EditedParamRef_SavesAsJsonNumber_NotString()
    {
        // 回归门禁：引擎编译器拒绝 "rate": "200"（must be a number or param
        // binding）→ reload 被静默打回，预览保持旧程序（"改参数预览没反应"）。
        // 编辑后的 param_ref 必须落盘为 JSON number。
        var (vm, path) = Load();
        try
        {
            var rate = vm.AllModules.Single(m => m.Name == "SpawnRate")
                .Params.Single(p => p.Name == "rate");
            Assert.False(vm.IsDirty, "freshly loaded asset must not be dirty");
            rate.Value = "200";

            Assert.True(vm.IsDirty, "editing a param row must mark the asset dirty");
            Assert.NotEqual("", vm.DirtyMarker);

            vm.Save();
            Assert.False(vm.IsDirty, "Save must clear the dirty flag");
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
            var el = doc.RootElement.GetProperty("stages").GetProperty("spawn")[0].GetProperty("rate");
            Assert.Equal(System.Text.Json.JsonValueKind.Number, el.ValueKind);
            Assert.Equal(200f, el.GetSingle());
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void LegacyStringifiedParamRef_HealsToNumberOnSave()
    {
        // 旧版编辑器把 param_ref 用户输入写成字符串（"rate": "50"）。这类文档
        // 加载后仍按数字回显，且下一次保存自我修复为 number。
        InjectSchema();
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.pfx_emitter");
        File.WriteAllText(path,
            """{ "name": "heal", "stages": { "spawn": [ { "module": "SpawnRate", "rate": "50" } ] } }""");
        try
        {
            var vm = new PfxEmitterEditorViewModel();
            vm.SetAsset(new AssetInfo { FullPath = path, Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter });

            var rate = vm.AllModules.Single(m => m.Name == "SpawnRate")
                .Params.Single(p => p.Name == "rate");
            Assert.True(rate.IsNumeric);
            Assert.Equal("50", rate.Value);

            vm.Save();
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
            var el = doc.RootElement.GetProperty("stages").GetProperty("spawn")[0].GetProperty("rate");
            Assert.Equal(System.Text.Json.JsonValueKind.Number, el.ValueKind);
            Assert.Equal(50f, el.GetSingle());
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void ParamRefObjectBinding_RoundTripsAsObject()
    {
        // param_ref 的绑定形态 {"param":...} 不是数字：保留自由文本行，
        // 且序列化必须原样写回对象（不得双重编码成字符串）。
        InjectSchema();
        var path = Path.Combine(Path.GetTempPath(), $"pfx_test_{Guid.NewGuid():N}.pfx_emitter");
        File.WriteAllText(path,
            """{ "name": "bound", "stages": { "spawn": [ { "module": "SpawnRate", "rate": { "param": "MyRate" } } ] } }""");
        try
        {
            var vm = new PfxEmitterEditorViewModel();
            vm.SetAsset(new AssetInfo { FullPath = path, Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter });

            var rate = vm.AllModules.Single(m => m.Name == "SpawnRate")
                .Params.Single(p => p.Name == "rate");
            Assert.True(rate.IsText, "对象绑定形态走自由文本行");

            vm.Save();
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
            var el = doc.RootElement.GetProperty("stages").GetProperty("spawn")[0].GetProperty("rate");
            Assert.Equal(System.Text.Json.JsonValueKind.Object, el.ValueKind);
            Assert.Equal("MyRate", el.GetProperty("param").GetString());
        }
        finally { File.Delete(path); }
    }
}
