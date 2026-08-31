using System;

namespace PrimalEditor_Avalonia.Common;

public static class Logger
{
    private static ConsoleLogger _console = new();
    private static UiLogger _ui = new();

    public static ILogger Current
    {
        get => _console;
        set
        {
            _console = value is ConsoleLogger cl ? cl : new ConsoleLogger();
        }
    }

    public static void Info(string message)
    {
        _console.Info(message);
        _ui.Info(message);
    }

    public static void Warning(string message)
    {
        _console.Warning(message);
        _ui.Warning(message);
    }

    public static void Error(string message, Exception? ex = null)
    {
        _console.Error(message, ex);
        _ui.Error(message, ex);
    }

    public static void Debug(string message)
    {
        _console.Debug(message);
        _ui.Debug(message);
    }
}