using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Models.PFX;

/// <summary>PfxGetEmitterSchema 返回的 JSON 反序列化模型（引擎→编辑器的模式反射）。</summary>
public sealed class PfxSchema
{
    [JsonPropertyName("version")]
    public int Version { get; set; }

    [JsonPropertyName("stages")]
    public List<string> Stages { get; set; } = new();

    [JsonPropertyName("standard_attributes")]
    public List<string> StandardAttributes { get; set; } = new();

    [JsonPropertyName("modules")]
    public List<PfxModuleSchema> Modules { get; set; } = new();
}

public sealed class PfxModuleSchema
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("stage")]
    public string Stage { get; set; } = "";

    [JsonPropertyName("params")]
    public List<PfxParamSchema> Params { get; set; } = new();
}

public sealed class PfxParamSchema
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("type")]
    public string Type { get; set; } = "f32";

    [JsonPropertyName("note")]
    public string Note { get; set; } = "";

    /// <summary>JSON 编码的引擎默认值（如 [0,1,0]、100）；null = 不补全。
    /// 编辑器据此为文档缺失的参数生成可编辑行。</summary>
    [JsonPropertyName("default")]
    public System.Text.Json.JsonElement? Default { get; set; }
}
