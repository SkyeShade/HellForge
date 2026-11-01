namespace HellForge.SteamDownload;

// Linux: SteamCMD asks for the password, and any Steam Guard code, on its terminal. The native launcher has none, so it
// opens a terminal window for SteamCMD, as the Windows launcher gives it a console window of its own; the player types
// into that window, and HellForge starts it and never reads from it (SteamCmd.RunInTerminalAsync). The terminal programs
// tried, in this order: the desktop's own choice through xdg-terminal-exec, KDE's Konsole, GNOME Terminal, GNOME Console,
// xterm, then other common ones. Each is started with the option that runs a program with its arguments exactly as given,
// so no shell splits or reads them. With none found on PATH, the SteamCMD way is not offered on that PC
// (DownloadChoices.Offered), and the card says why (DownloadChoices.NoTerminal).
public sealed record Terminal(string Name, string Program, IReadOnlyList<string> Options)
{
    // The command line that opens a window running command (a program and its arguments).
    public IReadOnlyList<string> CommandLine(IReadOnlyList<string> command) => [.. Options, .. command];
}

public static class SteamCmdTerminal
{
    // Each terminal's program name and the options before the program it runs. Only terminals whose option takes the
    // program and its arguments as separate arguments are here (tilix -e and lxterminal -e take one string a shell parses).
    public static IReadOnlyList<(string Name, string[] Options)> Known { get; } =
    [
        ("xdg-terminal-exec", []),
        ("konsole", ["-e"]),
        ("gnome-terminal", ["--"]),
        ("kgx", ["--"]),
        ("xterm", ["-e"]),
        ("ptyxis", ["--"]),
        ("xfce4-terminal", ["-x"]),
        ("mate-terminal", ["-x"]),
        ("terminator", ["-x"]),
        ("alacritty", ["-e"]),
        ("kitty", []),
        ("foot", []),
        ("wezterm", ["start", "--"]),
        ("x-terminal-emulator", ["-e"]),
    ];

    // The terminals on this PC, in Known's order: each program found in a PATH folder (path: PATH's value; null reads
    // PATH, or the usual folders when it is not set) and runnable (canRun: by default a file with an execute bit).
    public static IReadOnlyList<Terminal> Available(string? path = null, Func<string, bool>? canRun = null)
    {
        path ??= Environment.GetEnvironmentVariable("PATH") is { Length: > 0 } set ? set : "/usr/local/bin:/usr/bin:/bin";
        canRun ??= Runnable;
        var folders = path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries).Where(Path.IsPathRooted).ToList();
        var found = new List<Terminal>();
        foreach (var (name, options) in Known)
            if (folders.Select(folder => Path.Combine(folder, name)).FirstOrDefault(canRun) is { } program)
                found.Add(new Terminal(name, program, options));
        return found;
    }

    static bool Runnable(string path)
    {
        if (!File.Exists(path)) return false;
        if (OperatingSystem.IsWindows()) return false;
        const UnixFileMode execute = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
        try { return (File.GetUnixFileMode(path) & execute) != 0; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }
}
