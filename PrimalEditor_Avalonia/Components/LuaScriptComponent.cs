using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// 文件型 Lua 脚本组件（scripting S1，2026-10-11）：把 Content 里的 .lua 资产
/// 挂到实体上，Parameters 为字符串 KV（引擎侧 params 表，Lua 算术自然转型）。
/// 与 data-only 的 Rotate/Patrol 占位不同：本组件经
/// SceneRuntimeSynchronizer → EngineDLL ScriptCreateForEntity 真正执行，
/// 运行语义见 Engine/Scripting/LuaBackend（钩子 begin_play/update/…，
/// entity.* 表，弧度）。
/// </summary>
[JsonDerivedType(typeof(LuaScriptComponent), typeDiscriminator: "scriptLua")]
public sealed class LuaScriptComponent : ScriptableComponent
{
    public override string ScriptTypeId => "Lua";

    /// <summary>脚本资产（.lua）的注册表 GUID。</summary>
    [JsonPropertyName("ScriptAssetGuid")]
    public AssetGuid ScriptAssetGuid { get; set; }

    /// <summary>显示用名字（注册表解析失败时的回退标识）。</summary>
    [JsonPropertyName("ScriptName")]
    public string ScriptName { get; set; } = "";
}
