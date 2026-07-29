using System.Text.Json;

namespace PrimalEditor_Avalonia.Common;

public static class DockingLayoutSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public static string Save(DockingLayout layout) => JsonSerializer.Serialize(layout, Options);

    public static DockingLayout? Load(string json)
    {
        if (string.IsNullOrWhiteSpace(json)) return null;
        try
        {
            return JsonSerializer.Deserialize<DockingLayout>(json, Options);
        }
        catch
        {
            return null;
        }
    }
}
