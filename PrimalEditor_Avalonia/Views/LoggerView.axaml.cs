using System.Collections.Specialized;
using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class LoggerView : UserControl
{
    private ListBox? _logList;
    private LoggerViewModel? _viewModel;

    public LoggerView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
        _logList = this.FindControl<ListBox>("LogList");
    }

    private void OnDataContextChanged(object? sender, System.EventArgs e)
    {
        if (_viewModel != null)
        {
            _viewModel.FilteredEntries.CollectionChanged -= OnFilteredEntriesChanged;
            _viewModel.PropertyChanged -= OnViewModelPropertyChanged;
        }

        _viewModel = DataContext as LoggerViewModel;

        if (_viewModel != null)
        {
            _viewModel.FilteredEntries.CollectionChanged += OnFilteredEntriesChanged;
            _viewModel.PropertyChanged += OnViewModelPropertyChanged;
        }
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(LoggerViewModel.AutoScroll))
        {
            if (_viewModel?.AutoScroll == true)
            {
                ScrollToEnd();
            }
        }
        else if (e.PropertyName == nameof(LoggerViewModel.MinLevelIndex))
        {
            if (_viewModel?.AutoScroll == true)
            {
                Dispatcher.UIThread.Post(ScrollToEnd, DispatcherPriority.Background);
            }
        }
    }

    private void OnFilteredEntriesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (_viewModel?.AutoScroll != true) return;

        if (e.Action == NotifyCollectionChangedAction.Add ||
            e.Action == NotifyCollectionChangedAction.Reset)
        {
            Dispatcher.UIThread.Post(ScrollToEnd, DispatcherPriority.Background);
        }
    }

    private void ScrollToEnd()
    {
        if (_logList == null || _logList.ItemCount == 0) return;
        var lastIndex = _logList.ItemCount - 1;
        _logList.ScrollIntoView(lastIndex);
    }

    private void OnClearClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        UiLogger.Clear();
    }
}