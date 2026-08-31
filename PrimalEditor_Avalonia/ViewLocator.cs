using System;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Dock.Model.Core;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia;

public class ViewLocator : IDataTemplate
{
    public Control? Build(object? param)
    {
        if (param is null)
            return null;

        // For Dockable VMs (P1.d: WorldEditorViewModel and
        // ContentBrowserViewModel now extend Document which extends
        // DockBase, not ViewModelBase), use the Dockable.Id to look
        // up the corresponding View class by convention
        // ("FooDockable" / "FooViewModel" → "FooView"). Falls back
        // to a TextBlock placeholder if the convention fails.
        string name;
        if (param is IDockable dockable)
        {
            name = dockable.Id?.Replace("Dockable", "").Replace("ViewModel", "")
                ?? param.GetType().Name;
            // Try the resolved "WorldEditor" → "WorldEditorView"
            // convention first.
            foreach (var suffix in new[] { "View", "Control" })
            {
                var candidate = $"PrimalEditor_Avalonia.Views.{name}{suffix}";
                var t = Type.GetType(candidate);
                if (t != null && typeof(Control).IsAssignableFrom(t))
                    return (Control)Activator.CreateInstance(t)!;
            }
        }

        if (param is ViewModelBase)
        {
            name = param.GetType().FullName!.Replace("ViewModel", "View", StringComparison.Ordinal);
            var type = Type.GetType(name);
            if (type != null && typeof(Control).IsAssignableFrom(type))
                return (Control)Activator.CreateInstance(type)!;
        }

        return new TextBlock { Text = "Not Found: " + param.GetType().Name };
    }

    public bool Match(object? data)
    {
        if (data is null) return false;
        // P1.d: WorldEditorViewModel / ContentBrowserViewModel
        // extend Document (Dockable). Match IDockable so DockControl
        // can resolve a DataTemplate for the tab content. Without
        // this, DockControl renders an empty tab strip because no
        // template matches the Dockable type.
        return data is ViewModelBase || data is IDockable;
    }
}