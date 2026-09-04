using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Prefabs;

namespace PrimalEditor_Avalonia.Editors.Prefab;

/// <summary>
/// Read-only prefab inspector (plan §6.2 A4, v1): shows the prefab's
/// entity tree (names + component discriminators). Editing instances is
/// tracked separately; this panel only answers "what's inside".
/// </summary>
public sealed class PrefabInspectorViewModel : AssetEditorViewModel
{
    private List<PrefabEntityRow> _rows = new();
    public IReadOnlyList<PrefabEntityRow> Rows => _rows;

    private string _summary = "(no prefab loaded)";
    public string Summary
    {
        get => _summary;
        private set { if (_summary != value) { _summary = value; OnPropertyChanged(); } }
    }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        Reload();
    }

    public override void Discard() => Reload();

    private void Reload()
    {
        _rows = new List<PrefabEntityRow>();
        var prefab = Asset == null ? null : PrefabSerializer.Read(Asset.FullPath);
        if (prefab != null)
        {
            foreach (var doc in prefab.Entities)
            {
                var components = string.Join(", ",
                    doc.Components.Select(c => c.Type).DefaultIfEmpty("(no components)"));
                _rows.Add(new PrefabEntityRow(
                    doc.Name,
                    doc.ParentEntityGuid?.Value.ToString("D")[..8] ?? "(root)",
                    components));
            }
            Summary = $"{prefab.Name} — {prefab.Entities.Count} entity(ies), schema v{prefab.SchemaVersion}";
        }
        else
        {
            Summary = "(prefab unreadable)";
        }
        OnPropertyChanged(nameof(Rows));
    }
}

public sealed record PrefabEntityRow(string Name, string Parent, string Components);
