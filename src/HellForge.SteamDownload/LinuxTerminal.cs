namespace HellForge.SteamDownload;

// A terminal window program on Linux, for SteamCMD: the launcher has no terminal of its own, and SteamCMD asks for the
// password in one. Before: the program's arguments that come before the command to run, as its own manual gives them.
public sealed record LinuxTerminal(string Program, IReadOnlyList<string> Before)
{
    // In the order tried: the desktops' own terminals first, then the common others. Each takes the command as
    // separate arguments after these. Several hand the command to a terminal that is already open and end at once,
    // which SteamCmd.StartInTerminal allows for, so none needs an option to stay.
    public static readonly IReadOnlyList<(string Name, string[] Before)> Known =
    [
        ("konsole", ["-e"]),
        ("ptyxis", ["--"]),
        ("gnome-terminal", ["--"]),
        ("kgx", ["--"]),
        ("xfce4-terminal", ["-x"]),
        ("mate-terminal", ["-x"]),
        ("kitty", []),
        ("alacritty", ["-e"]),
        ("foot", []),
        ("wezterm", ["start", "--"]),
        ("xterm", ["-e"]),
    ];

    // The first known terminal on PATH, or null: SteamCMD is then not offered, and the other ways remain.
    public static LinuxTerminal? Find() => Find(Environment.GetEnvironmentVariable("PATH"), IsProgram);

    public static LinuxTerminal? Find(string? path, Func<string, bool> isProgram)
    {
        // Linux's PATH and paths, whatever system reads this: ":" between folders, "/" inside them.
        var folders = (path ?? "").Split(':', StringSplitOptions.RemoveEmptyEntries);
        foreach (var (name, before) in Known)
            foreach (var folder in folders)
                if (folder.StartsWith('/') && isProgram(folder.TrimEnd('/') + "/" + name))
                    return new(folder.TrimEnd('/') + "/" + name, before);
        return null;
    }

    static bool IsProgram(string file)
    {
        try
        {
            return !OperatingSystem.IsWindows() && File.Exists(file) &&
                (File.GetUnixFileMode(file) & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) != 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }
}
