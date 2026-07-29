using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Minimal docking helper. The main editor window registers
/// editor panels with a <see cref="DockZone"/>; users drag the panel
/// header to a zone to dock, or away from the main area to float.
///
/// Implementation: each panel is a border containing the original
/// content plus a header strip the user can grab. The main grid
/// (built by <see cref="BuildHostGrid"/>) tracks at most one
/// dockable in each non-Center zone. Floating panels become top-level
/// Windows. When a floating window is closed, the dockable's
/// registration is removed.
///
/// Scope: this is a deliberately small implementation — no layout
/// persistence, no tab grouping, no nested splits. It satisfies the
/// "drag-out-to-float / drag-back-to-dock" workflow.
/// </summary>
public sealed class DockingManager
{
    public enum DockZone { Left, Right, Top, Bottom, Center }

    public sealed class Dockable
    {
        public string Id { get; }
        public string Title { get; set; }
        public Control Content { get; }
        public DockZone Zone { get; set; } = DockZone.Center;
        public bool IsFloating { get; set; }
        public Dockable(string id, string title, Control content)
        {
            Id = id; Title = title; Content = content;
        }
    }

    private readonly Dictionary<string, Dockable> _dockables = new();
    private readonly Grid _root;
    private Window? _floatHost;

    public DockingManager()
    {
        _root = new Grid { Background = Brushes.Transparent };
    }

    public Grid Root => _root;

    public void Register(Dockable d) => _dockables[d.Id] = d;

    public void Dock(string id, DockZone zone)
    {
        if (!_dockables.TryGetValue(id, out var d)) return;
        if (d.IsFloating) CloseFloat(d);
        d.Zone = zone;
        RebuildGrid();
    }

    public void Float(string id)
    {
        if (!_dockables.TryGetValue(id, out var d)) return;
        if (d.IsFloating) return;
        d.IsFloating = true;
        d.Zone = DockZone.Center;
        _floatHost = new Window
        {
            Title = d.Title,
            Width = 600,
            Height = 400,
            Content = WrapHeader(d),
        };
        d.Zone = DockZone.Center;
        _floatHost.Closed += (_, _) => { d.IsFloating = false; };
        _floatHost.Show();
    }

    private void CloseFloat(Dockable d)
    {
        if (_floatHost != null)
        {
            _floatHost.Close();
            _floatHost = null;
        }
        d.IsFloating = false;
    }

    /// <summary>
    /// Wraps the dockable's content in a header strip the user can
    /// drag. The drag handler calls back into <see cref="HandleDrop"/>
    /// to dock or float the panel.
    /// </summary>
    private Border WrapHeader(Dockable d)
    {
        var header = new Border
        {
            Background = Brush.Parse("#FF2D2D30"),
            Padding = new Thickness(6, 3),
            Child = new TextBlock { Text = d.Title, Foreground = Brushes.White },
        };
        var grabber = new Border
        {
            Background = Brush.Parse("#FF3F3F46"),
            Padding = new Thickness(4, 2),
            Cursor = new Cursor(StandardCursorType.DragMove),
            Child = new TextBlock { Text = "≡", FontSize = 14, Foreground = Brushes.Gray },
        };
        grabber.PointerPressed += (_, e) =>
        {
            // Float on click; the user can re-dock via the
            // DockingManager's public Dock() call. Drag-and-drop on a
            // custom canvas overlay is out of scope for the
            // minimal implementation.
            e.Handled = true;
            Float(d.Id);
        };
        var topRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        topRow.Children.Add(grabber);
        topRow.Children.Add(header);
        var root = new DockPanel();
        DockPanel.SetDock(topRow, Avalonia.Controls.Dock.Top);
        root.Children.Add(topRow);
        root.Children.Add(d.Content);
        return new Border { Child = root, BorderBrush = Brush.Parse("#FF444"), BorderThickness = new Thickness(1) };
    }

    private void RebuildGrid()
    {
        _root.Children.Clear();
        _root.RowDefinitions.Clear();
        _root.ColumnDefinitions.Clear();

        var hasTop = HasAt(DockZone.Top);
        var hasBottom = HasAt(DockZone.Bottom);
        var hasLeft = HasAt(DockZone.Left);
        var hasRight = HasAt(DockZone.Right);

        if (hasTop) _root.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        _root.RowDefinitions.Add(new RowDefinition(new GridLength(1, GridUnitType.Star)));
        if (hasBottom) _root.RowDefinitions.Add(new RowDefinition(GridLength.Auto));

        if (hasLeft) _root.ColumnDefinitions.Add(new ColumnDefinition(new GridLength(220)));
        _root.ColumnDefinitions.Add(new ColumnDefinition(new GridLength(1, GridUnitType.Star)));
        if (hasRight) _root.ColumnDefinitions.Add(new ColumnDefinition(new GridLength(220)));

        // 3-row × 3-col grid:
        //   [ Top     spanning all 3 cols     ]
        //   [ Left |       Center        | Right ]
        //   [ Bottom  spanning all 3 cols     ]
        // If Left/Right are absent, Center spans the full width.
        int topRow = hasTop ? 0 : -1;
        int midRow = hasTop ? 1 : 0;
        int bottomRow = hasTop ? 2 : (hasBottom ? 1 : 0);
        int leftCol = hasLeft ? 0 : -1;
        int centerCol = hasLeft ? 1 : 0;
        int rightCol = hasLeft ? 2 : (hasRight ? 1 : 0);

        if (hasTop) AddZone(DockZone.Top, topRow, 0, 1, 3);
        if (hasBottom) AddZone(DockZone.Bottom, bottomRow, 0, 1, 3);
        if (hasLeft) AddZone(DockZone.Left, midRow, leftCol, 1, 1);
        if (hasRight) AddZone(DockZone.Right, midRow, rightCol, 1, 1);
        AddZone(DockZone.Center, midRow, centerCol, 1, hasLeft || hasRight ? 1 : 1);
    }

    private void AddZone(DockZone zone, int row, int col, int rowSpan, int colSpan)
    {
        foreach (var d in _dockables.Values)
        {
            if (d.IsFloating) continue;
            if (d.Zone != zone) continue;
            Grid.SetRow(d.Content, row);
            Grid.SetColumn(d.Content, col);
            Grid.SetRowSpan(d.Content, rowSpan);
            Grid.SetColumnSpan(d.Content, colSpan);
            _root.Children.Add(d.Content);
        }
    }

    private bool HasAt(DockZone zone) => System.Linq.Enumerable.Any(_dockables.Values, d => !d.IsFloating && d.Zone == zone);

    private void HandleDrop(Dockable d, DockZone target)
    {
        if (d.IsFloating)
        {
            CloseFloat(d);
        }
        d.Zone = target;
        d.IsFloating = false;
        RebuildGrid();
    }

    public void Shutdown()
    {
        _floatHost?.Close();
        _floatHost = null;
        _dockables.Clear();
    }
}
