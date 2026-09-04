using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia.Data;
using Avalonia.Data.Converters;
using PrimalEditor_Avalonia.Themes;
using PrimalEditor_Avalonia.Themes.Controls;

namespace PrimalEditor_Avalonia.Converters;

/// <summary>
/// 材质节点 → 阵营外框(Warcraft 主题专属装饰)。
/// 按节点 TypeName 分配阵营框: 数学类=兽人、纹理类=精灵、
/// 时间/法线类=亡灵、常量类=人类、其余=中立。
/// 非 Warcraft 主题返回 null → Frame 附加行为不挂载, 其它主题不受影响。
/// </summary>
public static class MaterialAvatarConverters
{
    /// <summary>节点外框贴图 URI(Warcraft 专属): 按 TypeName 选阵营框;
    /// 其它主题返回 null → Frame 附加行为不挂载, 节点保持 NodifyM 默认外观。</summary>
    public static readonly IValueConverter FrameForNode = new NodeFrameConverter();

    private sealed class NodeFrameConverter : IValueConverter
    {
        public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            if (ThemeManager.Current.Id != EditorThemeRegistry.WarcraftId) return null;
            var typeName = (value as string)?.Trim() ?? string.Empty;
            var faction = FactionFor(typeName);
            return $"avares://PrimalEditor_Avalonia/Assets/Textures/Warcraft/node-frame-{faction}.png";
        }

        public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }

    // TypeName(调色板次级文本) → 阵营
    private static readonly Dictionary<string, string> FactionByType = new()
    {
        ["Add"] = "orc",
        ["Multiply"] = "orc",
        ["Pow"] = "orc",
        ["Select"] = "orc",
        ["ConstantTexture"] = "elf",
        ["SampleTexture"] = "elf",
        ["Time"] = "undead",
        ["NormalBlend"] = "undead",
        ["ConstantFloat"] = "human",
        ["ConstantFloat2"] = "human",
        ["ConstantFloat3"] = "human",
        ["ConstantFloat4"] = "human",
        ["Lerp"] = "human",
        ["Clamp"] = "human",
        ["Saturate"] = "human",
        ["Fresnel"] = "human",
    };

    private static string FactionFor(string typeName)
        => FactionByType.TryGetValue(typeName, out var faction) ? faction : "default";

}
