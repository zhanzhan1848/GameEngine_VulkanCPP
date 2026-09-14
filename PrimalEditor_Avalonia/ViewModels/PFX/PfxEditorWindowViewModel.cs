using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Models.PFX;
using PrimalEditor_Avalonia.Services;

namespace PrimalEditor_Avalonia.ViewModels.PFX;

// ---- 参数编辑行 ---------------------------------------------------------------

/// <summary>单参数编辑行——按 Type 分发控件（f32→TextBox、enum→ComboBox、curve→JSON 文本）。</summary>
public sealed class PfxParamField : INotifyPropertyChanged
{
    private string _value = "";

    public string Name { get; }
    public string Type { get; }
    public string Note { get; }

    // 类型分发标志（schema 驱动 AXAML 控件切换）
    public bool IsEnum { get; }
    public bool IsF32 => !IsEnum;

    public string Value
    {
        get => _value;
        set { _value = value; Fire(nameof(Value)); }
    }

    public PfxParamField(string name, string type, string note, string initial)
    {
        Name = name; Type = type; Note = note; _value = initial;
        IsEnum = type == "enum";
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Fire(string name) => PropertyChanged?.Invoke(this, new(name));
}

/// <summary>模块实例行（阶段 + 名称 + 参数集合）。</summary>
public sealed class PfxModuleRow : INotifyPropertyChanged
{
    private string _name = "";
    private bool _selected;

    public string Name { get => _name; set { _name = value; Fire(nameof(Name)); } }
    public bool Selected { get => _selected; set { _selected = value; Fire(nameof(Selected)); } }
    public string Stage { get; init; } = "";

    public ObservableCollection<PfxParamField> Params { get; } = new();

    public static PfxModuleRow FromJson(string stage, JsonNode? node)
    {
        var row = new PfxModuleRow { Stage = stage };
        if (node is JsonObject obj)
        {
            if (obj["module"] is JsonValue mv && mv.TryGetValue<string>(out var mn))
                row.Name = mn;
            foreach (var (key, val) in obj)
            {
                if (key == "module") continue;
                row.Params.Add(new PfxParamField(key, "f32", "", val?.ToJsonString().Trim('"') ?? ""));
            }
        }
        return row;
    }

    public JsonObject ToJson()
    {
        var obj = new JsonObject { ["module"] = Name };
        foreach (var p in Params)
        {
            if (float.TryParse(p.Value, out var f)) obj[p.Name] = f;
            else if (uint.TryParse(p.Value, out var u)) obj[p.Name] = u;
            else obj[p.Name] = p.Value;
        }
        return obj;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Fire(string name) => PropertyChanged?.Invoke(this, new(name));
}

// ---- 窗口 VM -------------------------------------------------------------------

/// <summary>发射器编辑器窗口 VM（R2：schema 驱动动态表单 + Undo/Redo + 热预览 + Save）。</summary>
public sealed class PfxEditorWindowViewModel : INotifyPropertyChanged
{
    private string _assetPath = "";
    private string _jsonText = "{}";
    private string _statusText = "Ready";
    private string _selectedStage = "spawn";
    private PfxModuleRow? _selectedModule;
    private readonly Stack<string> _undoStack = new();
    private readonly Stack<string> _redoStack = new();
    private DateTime _lastPush = DateTime.MinValue;

    public static uint CurrentEmitterId { get; set; }

    // ---- schema 缓存 ----
    public PfxSchema? Schema { get; private set; }

    // ---- 集合 ----
    public ObservableCollection<string> StageNames { get; } = new() { "spawn", "init", "update" };
    public ObservableCollection<PfxModuleRow> AllModules { get; } = new();

    public string AssetPath { get => _assetPath; set { _assetPath = value; Fire(nameof(AssetPath)); } }
    public string JsonText { get => _jsonText; set { _jsonText = value; Fire(nameof(JsonText)); } }
    public string StatusText { get => _statusText; set { _statusText = value; Fire(nameof(StatusText)); } }
    public string SelectedStage
    {
        get => _selectedStage;
        set { _selectedStage = value; Fire(nameof(SelectedStage)); Fire(nameof(VisibleModules)); }
    }

    public IEnumerable<PfxModuleRow> VisibleModules =>
        AllModules.Where(m => m.Stage == SelectedStage);

    public PfxModuleRow? SelectedModule
    {
        get => _selectedModule;
        set
        {
            _selectedModule = value;
            Fire(nameof(SelectedModule));
            Fire(nameof(SelectedModuleParams));
        }
    }

    public ObservableCollection<PfxParamField> SelectedModuleParams =>
        SelectedModule?.Params ?? new();

    // ---- 命令 ----
    public ICommand SaveCommand { get; }
    public ICommand HotPreviewCommand { get; }
    public ICommand UndoCommand { get; }
    public ICommand RedoCommand { get; }
    public ICommand RemoveModuleCommand { get; }

    // ---- 生命周期 ----

    public PfxEditorWindowViewModel(string assetPath, string jsonText)
    {
        _assetPath = assetPath;
        LoadJson(jsonText);
        SaveCommand = new RelayCommand(_ => Save());
        HotPreviewCommand = new RelayCommand(_ => PushHotPreview());
        UndoCommand = new RelayCommand(_ => Undo(), _ => _undoStack.Count > 1);
        RedoCommand = new RelayCommand(_ => Redo(), _ => _redoStack.Count > 0);
        RemoveModuleCommand = new RelayCommand(_ => RemoveSelected(), _ => SelectedModule != null);
    }

    public void LoadJson(string json)
    {
        _jsonText = json;
        AllModules.Clear();
        try
        {
            var root = JsonNode.Parse(json);
            var stages = root?["stages"] as JsonObject;
            if (stages == null) return;
            foreach (var sn in new[] { "spawn", "init", "update" })
            {
                if (stages[sn] is not JsonArray arr) continue;
                foreach (var mod in arr.OfType<JsonObject>())
                {
                    var name = mod["module"]?.GetValue<string>() ?? "?";
                    var row = new PfxModuleRow { Name = name, Stage = sn };
                    foreach (var (key, val) in mod)
                    {
                        if (key == "module") continue;
                        row.Params.Add(new PfxParamField(key, "f32", "", val?.ToJsonString().Trim('"') ?? ""));
                    }
                    AllModules.Add(row);
                }
            }
        }
        catch { }
        SelectedStage = "spawn";
    }

    // ---- 编辑操作 ----

    public void AddModule(string stage, string moduleName)
    {
        var row = new PfxModuleRow { Name = moduleName, Stage = stage };
        AllModules.Add(row);
        SyncFromModules();
        SelectedStage = stage;
        Fire(nameof(VisibleModules));
        PushHotPreview();
    }

    public void RemoveSelected()
    {
        if (SelectedModule == null) return;
        AllModules.Remove(SelectedModule);
        SyncFromModules();
        SelectedModule = null;
        Fire(nameof(VisibleModules));
        PushHotPreview();
    }

    /// <summary>模块行 → JSON 同步 → 热预览推送。</summary>
    public void SyncToJson()
    {
        var root = JsonNode.Parse(_jsonText) as JsonObject ?? new JsonObject();
        var stages = root["stages"] as JsonObject ?? new JsonObject();
        root["stages"] = stages;

        foreach (var stage in new[] { "spawn", "init", "update" })
        {
            var arr = new JsonArray();
            foreach (var mod in AllModules.Where(m => m.Stage == stage))
            {
                var obj = new JsonObject { ["module"] = mod.Name };
                foreach (var p in mod.Params)
                {
                    if (float.TryParse(p.Value, out var f)) obj[p.Name] = f;
                    else if (uint.TryParse(p.Value, out var u)) obj[p.Name] = u;
                    else obj[p.Name] = p.Value;
                }
                arr.Add(obj);
            }
            stages[stage] = arr;
        }
        _jsonText = root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        Fire(nameof(JsonText));
        PushHotPreview();
    }

    // ---- Save / HotPreview / Undo / Redo ----

    public void Save()
    {
        _undoStack.Push(_jsonText);
        _redoStack.Clear();
        try
        {
            System.IO.File.WriteAllText(_assetPath, _jsonText);
            StatusText = "Saved " + DateTime.Now.ToString("HH:mm:ss");
        }
        catch (Exception ex) { StatusText = "Save failed: " + ex.Message; }
        PushHotPreview();
    }

    public void PushHotPreview()
    {
        var emitterId = CurrentEmitterId;
        if (emitterId == 0) return;
        if ((DateTime.Now - _lastPush).TotalMilliseconds < 300) return;
        _lastPush = DateTime.Now;
        var bytes = System.Text.Encoding.UTF8.GetBytes(_jsonText);
        PrimalEditor_Avalonia.Native.NativeEngineBridge.PfxEmitterReloadFromMemory(emitterId, bytes);
    }

    public void Undo()
    {
        if (_undoStack.Count < 2) return;
        _redoStack.Push(_undoStack.Pop());
        var prev = _undoStack.Peek();
        LoadJson(prev);
        Fire(nameof(JsonText));
        PushHotPreview();
    }

    public void Redo()
    {
        if (_redoStack.Count == 0) return;
        _undoStack.Push(_redoStack.Pop());
        var next = _redoStack.Peek();
        LoadJson(next);
        Fire(nameof(JsonText));
        PushHotPreview();
    }

    // ---- INPC ----

    private void SyncFromModules()
    {
        var root = JsonNode.Parse(_jsonText) as JsonObject ?? new JsonObject();
        var stages = root["stages"] as JsonObject ?? new JsonObject();
        root["stages"] = stages;
        foreach (var stage in new[] { "spawn", "init", "update" })
        {
            var arr = new JsonArray();
            foreach (var mod in AllModules.Where(m => m.Stage == stage))
            {
                var obj = new JsonObject { ["module"] = mod.Name };
                foreach (var p in mod.Params)
                {
                    if (float.TryParse(p.Value, out var f)) obj[p.Name] = f;
                    else if (uint.TryParse(p.Value, out var u)) obj[p.Name] = u;
                    else obj[p.Name] = p.Value;
                }
                arr.Add(obj);
            }
            stages[stage] = arr;
        }
        _jsonText = root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        Fire(nameof(JsonText));
    }

    private void Fire(string name) => PropertyChanged?.Invoke(this, new(name));

    public event PropertyChangedEventHandler? PropertyChanged;
}
