using System;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.ViewModels;

public class LogLevelToColorConverter : IValueConverter
{
    public static readonly LogLevelToColorConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return value switch
        {
            LogLevel.Debug => new SolidColorBrush(Color.FromRgb(128, 128, 128)),
            LogLevel.Info => new SolidColorBrush(Color.FromRgb(220, 220, 220)),
            LogLevel.Warning => new SolidColorBrush(Color.FromRgb(255, 200, 100)),
            LogLevel.Error => new SolidColorBrush(Color.FromRgb(255, 100, 100)),
            _ => new SolidColorBrush(Colors.White),
        };
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

public class LoggerViewModel : ViewModelBase
{
    private bool _autoScroll = true;
    private int _minLevelIndex = 1;

    public ObservableCollection<LogEntry> FilteredEntries { get; } = new();

    public bool AutoScroll
    {
        get => _autoScroll;
        set
        {
            if (_autoScroll != value)
            {
                _autoScroll = value;
                OnPropertyChanged(nameof(AutoScroll));
            }
        }
    }

    public int MinLevelIndex
    {
        get => _minLevelIndex;
        set
        {
            if (_minLevelIndex != value)
            {
                _minLevelIndex = value;
                OnPropertyChanged(nameof(MinLevelIndex));
                RefreshEntries();
            }
        }
    }

    public LoggerViewModel()
    {
        UiLogger.Entries.CollectionChanged += OnUiLoggerEntriesChanged;
        RefreshEntries();
    }

    private void OnUiLoggerEntriesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        RefreshEntries();
    }

    private void RefreshEntries()
    {
        var minLevel = (LogLevel)_minLevelIndex;
        FilteredEntries.Clear();
        foreach (var entry in UiLogger.Entries)
        {
            if ((int)entry.Level >= (int)minLevel)
            {
                FilteredEntries.Add(entry);
            }
        }
    }
}