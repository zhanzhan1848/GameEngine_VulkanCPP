using System.Text.RegularExpressions;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Parses compiler / linker diagnostic lines that follow the common
/// <c>file:line:col: severity: message</c> shape (gcc/clang/msvc). Returns
/// null when no recognizable diagnostic is in the input.
/// </summary>
public static class BuildErrorParser
{
    public readonly struct Hit
    {
        public string File { get; }
        public int Line { get; }
        public int Column { get; }
        public Hit(string file, int line, int column) { File = file; Line = line; Column = column; }
    }

    // gcc/clang/MSVC shape: <path>:<line>:<col>: <severity>: <message>
    // The severity token is optional in some toolchains, so we accept
    // both with and without it. We capture the path as non-greedy
    // up to the first colon, then line:col.
    private static readonly Regex Pattern = new(
        @"(?<file>(?:[A-Za-z]:)?[^:\r\n]+?):(?<line>\d+):(?<col>\d+):",
        RegexOptions.Compiled);

    public static Hit? Parse(string text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        var m = Pattern.Match(text);
        if (!m.Success) return null;
        if (!int.TryParse(m.Groups["line"].Value, out var line)) return null;
        if (!int.TryParse(m.Groups["col"].Value, out var col)) return null;
        var file = m.Groups["file"].Value.Trim();
        return new Hit(file, line, col);
    }
}
