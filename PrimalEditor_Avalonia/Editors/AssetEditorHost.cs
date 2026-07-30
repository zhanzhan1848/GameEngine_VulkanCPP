using System;
using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Views;
using L = PrimalEditor_Avalonia.Common.WindowState;

namespace PrimalEditor_Avalonia.Editors;

public class AssetEditorHost : Window
{
    public AssetEditorViewModel ViewModel { get; }

    private readonly AssetEditorStatusBar _statusBar = new();
    private L? _initialWindowState;
    private DispatcherTimer? _saveLayoutTimer;

    public AssetEditorHost(AssetEditorViewModel vm, L? initialState = null)
    {
        ViewModel = vm;
        _initialWindowState = initialState;
        DataContext = vm;
        ApplyInitialBounds();
        Content = BuildLayout(vm);
        InstallKeyboardShortcuts();
        vm.PropertyChanged += OnViewModelPropertyChanged;
        UpdateTitle();
    }

    private void ApplyInitialBounds()
    {
        if (_initialWindowState != null)
        {
            var s = _initialWindowState;
            WindowStartupLocation = WindowStartupLocation.Manual;
            Position = new Avalonia.PixelPoint(s.X, s.Y);
            Width = s.W;
            Height = s.H;
            WindowState = s.State == 1 ? Avalonia.Controls.WindowState.Maximized : Avalonia.Controls.WindowState.Normal;
        }
        else
        {
            Width = 900;
            Height = 700;
        }
    }

    private object BuildLayout(AssetEditorViewModel vm)
    {
        var view = BuildView(vm);
        if (view is null) return new TextBlock { Text = $"Editor for {vm.GetType().Name} not yet implemented" };
        var dock = new DockPanel();
        _statusBar.ViewModel = vm;
        DockPanel.SetDock(_statusBar, Avalonia.Controls.Dock.Bottom);
        dock.Children.Add(_statusBar);
        dock.Children.Add(view);
        return dock;
    }

    private Control? BuildView(AssetEditorViewModel vm) => vm switch
    {
        Texture.TextureEditorViewModel => new Texture.TextureEditorView { DataContext = vm },
        Geometry.GeometryEditorViewModel => new Geometry.GeometryEditorView { DataContext = vm },
        PrimitiveMesh.PrimitiveMeshViewModel => new PrimitiveMesh.PrimitiveMeshView { DataContext = vm },
        _ => null,
    };

    private void InstallKeyboardShortcuts()
    {
        // Avalonia 11 does not have a fully-featured KeyBindings collection
        // in the same shape as WPF; route via KeyDown event with explicit
        // modifier checks instead.
        KeyDown += OnHostKeyDown;
    }

    private void OnHostKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        if (ViewModel == null) return;
        bool ctrl = (e.KeyModifiers & KeyModifiers.Control) != 0;
        bool shift = (e.KeyModifiers & KeyModifiers.Shift) != 0;
        if (ctrl && e.Key == Key.S)
        {
            if (shift) ViewModel.Discard();
            else ViewModel.Save();
            e.Handled = true;
        }
        else if (ctrl && e.Key == Key.W)
        {
            Close();
            e.Handled = true;
        }
        else if (ctrl && !shift && e.Key == Key.Z)
        {
            // Undo the last settings change inside this editor.
            ViewModel.UndoRedo.Undo();
            e.Handled = true;
        }
        else if (ctrl && shift && e.Key == Key.Z)
        {
            // Redo the previously-undone change.
            ViewModel.UndoRedo.Redo();
            e.Handled = true;
        }
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(AssetEditorViewModel.IsDirty)
            || e.PropertyName == nameof(AssetEditorViewModel.Asset)
            || e.PropertyName == nameof(AssetEditorViewModel.Title))
        {
            UpdateTitle();
        }
    }

    private void UpdateTitle()
    {
        var vm = ViewModel;
        if (vm == null) return;
        var baseTitle = vm.Title ?? "(no asset)";
        var suffix = vm.IsDirty ? " *" : "";
        Title = $"{baseTitle}{suffix} — PrimalEditor";
    }

    protected override void OnClosed(System.EventArgs e)
    {
        SaveLayoutNow();
        base.OnClosed(e);
        if (ViewModel != null) ViewModel.PropertyChanged -= OnViewModelPropertyChanged;
    }

    protected override async void OnClosing(WindowClosingEventArgs e)
    {
        base.OnClosing(e);
        if (ViewModel == null || !ViewModel.IsDirty) return;

        e.Cancel = true;
        var dlg = ConfirmDialog.Create(
            "Unsaved changes",
            $"'{ViewModel.Asset?.FileName ?? "asset"}' has unsaved changes. Save before closing?");
        var result = await dlg.ShowDialogAsync(this);
        switch (result)
        {
            case ConfirmResult.Save:
                ViewModel.Save();
                Close();
                break;
            case ConfirmResult.Discard:
                Close();
                break;
            case ConfirmResult.Cancel:
            case null:
                break;
        }
    }

    public void ScheduleLayoutSave()
    {
        if (_saveLayoutTimer == null)
        {
            _saveLayoutTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            _saveLayoutTimer.Tick += (_, _) =>
            {
                _saveLayoutTimer!.Stop();
                SaveLayoutNow();
            };
        }
        _saveLayoutTimer.Stop();
        _saveLayoutTimer.Start();
    }

    private void SaveLayoutNow()
    {
        if (ViewModel?.Asset == null) return;
        var layout = EditorLayoutSettings.Load();
        var s = new L
        {
            X = Position.X,
            Y = Position.Y,
            W = (int)Width,
            H = (int)Height,
            State = WindowState == Avalonia.Controls.WindowState.Maximized ? 1 : 0,
        };
        layout.AssetWindows[ViewModel.Asset.FullPath] = s;
        if (!layout.OpenAssets.Contains(ViewModel.Asset.FullPath))
        {
            layout.OpenAssets.Add(ViewModel.Asset.FullPath);
        }
        EditorLayoutSettings.Save(layout);
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.Property == BoundsProperty || e.Property == WindowStateProperty)
        {
            ScheduleLayoutSave();
        }
    }
}
