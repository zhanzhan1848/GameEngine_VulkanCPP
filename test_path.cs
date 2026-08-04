using System;
using System.IO;

var paths = new[] { "/absolute/path", "C:\\absolute\\path", "relative/path", "folder\\subfolder\\file.txt" };
foreach (var path in paths) {
    Console.WriteLine($"{path} IsPathRooted: {Path.IsPathRooted(path)}");
}