using System;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Dock.Model.Core;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia;

public class ViewLocator : IDataTemplate
{
    // Dockable Id → 已创建的视图实例（仅 UI 线程访问）。Dock 标签页往返
    // 复用同一视图实例：WorldEditorView 内嵌的 EngineViewport3D 持有原生
    // Metal 表面与相机状态，每次重建实例都会走"新建表面"路径（重复
    // CreateEmbeddedRenderSurface + 相机位姿丢失）。缓存后切回标签只是
    // 同一控件的 detach → re-attach。
    private static readonly System.Collections.Generic.Dictionary<string, Control> s_dockViewCache = new();

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
                {
                    // Parent 非空（分屏同显等场景）时退回新建实例，避免同一
                    // 控件被挂到两个视觉父级。
                    var cacheKey = dockable.Id ?? name;
                    if (s_dockViewCache.TryGetValue(cacheKey, out var cached)
                        && cached.Parent == null)
                    {
                        return cached;
                    }
                    var instance = (Control)Activator.CreateInstance(t)!;
                    s_dockViewCache[cacheKey] = instance;
                    return instance;
                }
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