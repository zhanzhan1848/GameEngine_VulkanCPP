using System;

namespace PrimalEditor_Avalonia.Common;

public class ConsoleLogger : ILogger
{
    public void Info(string message)
    {
        var timestamp = DateTime.Now.ToString("HH:mm:ss");
        Console.ForegroundColor = ConsoleColor.White;
        Console.WriteLine($"[{timestamp}] [INFO] {message}");
        Console.ResetColor();
    }

    public void Warning(string message)
    {
        var timestamp = DateTime.Now.ToString("HH:mm:ss");
        Console.ForegroundColor = ConsoleColor.Yellow;
        Console.WriteLine($"[{timestamp}] [WARN] {message}");
        Console.ResetColor();
    }

    public void Error(string message, Exception? ex = null)
    {
        var timestamp = DateTime.Now.ToString("HH:mm:ss");
        Console.ForegroundColor = ConsoleColor.Red;
        Console.WriteLine($"[{timestamp}] [ERROR] {message}");
        if (ex != null)
        {
            Console.WriteLine($"  Exception: {ex.GetType().FullName}: {ex.Message}");
            if (!string.IsNullOrEmpty(ex.StackTrace))
            {
                Console.WriteLine($"  StackTrace:");
                foreach (var line in ex.StackTrace.Split('\n'))
                    Console.WriteLine($"    {line.TrimEnd()}");
            }
            var inner = ex.InnerException;
            while (inner != null)
            {
                Console.WriteLine($"  Inner: {inner.GetType().FullName}: {inner.Message}");
                if (!string.IsNullOrEmpty(inner.StackTrace))
                {
                    Console.WriteLine($"    InnerStackTrace:");
                    foreach (var line in inner.StackTrace.Split('\n'))
                        Console.WriteLine($"      {line.TrimEnd()}");
                }
                inner = inner.InnerException;
            }
        }
        Console.ResetColor();
    }

    public void Debug(string message)
    {
        var timestamp = DateTime.Now.ToString("HH:mm:ss");
        Console.ForegroundColor = ConsoleColor.Gray;
        Console.WriteLine($"[{timestamp}] [DEBUG] {message}");
        Console.ResetColor();
    }
}