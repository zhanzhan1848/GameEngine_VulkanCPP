namespace PrimalEditor_Avalonia.Common;

public interface IFileMover
{
    void Copy(string source, string dest, bool overwrite);
    void Move(string source, string dest, bool overwrite);
}

public sealed class SystemFileMover : IFileMover
{
    public void Copy(string source, string dest, bool overwrite) =>
        System.IO.File.Copy(source, dest, overwrite);
    public void Move(string source, string dest, bool overwrite) =>
        System.IO.File.Move(source, dest, overwrite);
}
