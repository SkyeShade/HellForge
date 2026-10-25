using System.ComponentModel;
using System.Diagnostics;
using HellForge.Core;

namespace HellForge.Session;

// The processes that keep a session from starting (PlaySession.Prepare): the normal game, GameGuard and its monitor.
// With HELLFORGE_MULTI_INSTANCE=1, a game another HellForge installation runs does not count: several installations on
// one PC each run a game of their own (docs/multi-instance.md). A game of this installation, or one whose path cannot be
// read, still does.
public static class GameProcesses
{
    public const string MultiInstanceVariable = "HELLFORGE_MULTI_INSTANCE";
    public const string Refusal = "Existing game or GameGuard process.";
    static readonly string[] Names = ["helldivers2", "GameGuard", "GameMon", "GameMon64"];

    public static bool MultiInstance => Environment.GetEnvironmentVariable(MultiInstanceVariable) == "1";

    // Why a session for this installation cannot start beside these processes (name, image path or null), or null.
    public static string? Blocking(InstallLayout install, IEnumerable<(string Name, string? Path)> running, bool multiInstance)
    {
        foreach (var (name, path) in running)
        {
            if (!name.Equals("helldivers2", StringComparison.OrdinalIgnoreCase)) return Refusal;
            if (!multiInstance || OtherInstallation(install, path) == null) return Refusal;
        }
        return null;
    }

    // The root of the HellForge installation whose game copy this is (<root>\game\bin\helldivers2.exe, with the
    // installation's marker beside game\), when that is not this installation; otherwise null.
    public static string? OtherInstallation(InstallLayout install, string? exePath)
    {
        if (exePath == null) return null;
        var bin = Path.GetDirectoryName(Path.GetFullPath(exePath));
        var game = bin == null ? null : Path.GetDirectoryName(bin);
        var root = game == null ? null : Path.GetDirectoryName(game);
        if (root == null || !Path.GetFileName(exePath).Equals("helldivers2.exe", StringComparison.OrdinalIgnoreCase) ||
            !Path.GetFileName(bin)!.Equals("bin", StringComparison.OrdinalIgnoreCase) ||
            !Path.GetFileName(game)!.Equals("game", StringComparison.OrdinalIgnoreCase)) return null;
        var other = new InstallLayout(root);
        if (!other.Exists) return null;
        return string.Equals(other.Root.TrimEnd('\\'), install.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase) ? null : other.Root;
    }

    // The game and GameGuard processes running now, with their image paths (null when unreadable).
    public static List<(string Name, string? Path)> Running()
    {
        var running = new List<(string, string?)>();
        foreach (var name in Names)
            foreach (var process in Process.GetProcessesByName(name))
                using (process) running.Add((name, ImagePath(process)));
        return running;
    }

    static string? ImagePath(Process process)
    {
        try { return process.MainModule?.FileName; }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException) { return null; }
    }
}
