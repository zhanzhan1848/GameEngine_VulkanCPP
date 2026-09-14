using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Models.PFX;

namespace PrimalEditor_Avalonia.Services;

/// <summary>
/// PFX 编辑器共享服务（Singleton）：schema 缓存 + 资产扫描 + 热预览推送。
/// schema 由引擎反射导出（PfxGetEmitterSchema），C# 侧只消费不硬编码。
/// v1 先用 C# 侧已知模块清单（与 BuiltinModules.cpp 注册一致）——
/// PfxGetEmitterSchema P/Invoke 接入后切换到引擎返回值。
/// </summary>
public sealed class PfxEditorService
{
    private static PfxEditorService? _instance;
    public static PfxEditorService Instance => _instance ??= new();

    public PfxSchema? Schema { get; private set; }
    public string AssetDirectory { get; set; } = ".";

    /// <summary>引擎反射 schema（返回 null = 未加载）。</summary>
    public string? GetSchemaJson() => PrimalEditor_Avalonia.Native.NativeEngineBridge.PfxGetEmitterSchema();

    /// <summary>解析并缓存 schema（从引擎 JSON）。</summary>
    public bool LoadSchema()
    {
        var json = GetSchemaJson();
        if (json == null) return false;
        try
        {
            Schema = System.Text.Json.JsonSerializer.Deserialize<PfxSchema>(json,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            return Schema != null;
        }
        catch { return false; }
    }

    /// <summary>扫描目录下所有 .pfx_emitter 文件。</summary>
    public List<PfxAssetEntry> ScanAssets(string directory)
    {
        var result = new List<PfxAssetEntry>();
        if (!Directory.Exists(directory)) return result;
        try
        {
            foreach (var f in Directory.GetFiles(directory, "*.pfx_emitter", SearchOption.TopDirectoryOnly))
                result.Add(new PfxAssetEntry { FullPath = Path.GetFullPath(f) });
        }
        catch { /* 权限等 IO 错误静默 */ }
        return result;
    }

    /// <summary>读取 .pfx_emitter JSON 全文。</summary>
    public string? ReadAsset(string path)
    {
        try { return File.ReadAllText(path); } catch { return null; }
    }

    /// <summary>写 .pfx_emitter 文件。</summary>
    public bool WriteAsset(string path, string json)
    {
        try { File.WriteAllText(path, json); return true; }
        catch { return false; }
    }
}
