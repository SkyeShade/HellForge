using System.Text.Json;

namespace HellForge.Core;

// A version picked to play with: its app/ but the launcher (which is always the newest), and its own data. Older versions
// read their programs, bundled mods, mod SDK and stand-ins at <installation>/app by fixed path, so the version played
// with is in app/; the installed version's files and data wait in app-versions/.installed meanwhile. Only renames, each
// journalled as a switch's are: a failure or a kill puts every one back. The newest launcher keeps running throughout.
public static partial class KeptVersions
{
    // The installed version's app files and data while a kept one is played with; its left.json names the mods it saw.
    public const string InstalledFolder = ".installed";
    const string LeftName = "left.json";
    // What stays in app/ whichever version plays: the launcher that is running.
    static readonly string[] Stays = ["launcher"];

    public static string InstalledPath(InstallLayout install) => Path.Combine(Root(install), InstalledFolder);

    // The version updates installed: in app/ unless a kept one is played with.
    public static AppIdentity Installed(InstallLayout install) =>
        Identify(Directory.Exists(Path.Combine(InstalledPath(install), "app")) ? Path.Combine(InstalledPath(install), "app") : install.App);

    // The version sessions play with now (its app-build.json is in app/ either way).
    public static AppIdentity Active(InstallLayout install) => Identify(install.App);

    public static bool PlaysKept(InstallLayout install) => Directory.Exists(InstalledPath(install));

    // Version: the version played with (0.3.0-beta's app-build.json does not say it).
    sealed record Left(List<string> Mods, DateTime LeftUtc, string? Version = null);

    // The version of the kept app in app/ while it is played with, from left.json; null otherwise.
    static string? PlayedVersion(string app)
    {
        var full = LocalInstall.FullPath(app);
        if (!string.Equals(Path.GetFileName(full), "app", StringComparison.OrdinalIgnoreCase) || Path.GetDirectoryName(full) is not { } root) return null;
        var left = Path.Combine(root, FolderName, InstalledFolder, LeftName);
        try { return File.Exists(left) ? JsonSerializer.Deserialize<Left>(File.ReadAllText(left), CopyPreparer.JsonOptions)?.Version : null; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // Why a kept version cannot be played with, or null.
    public static string? RuntimeProblem(InstallLayout install, KeptVersion target)
    {
        if (!Directory.Exists(install.App)) return "This installation has no app folder.";
        if (Active(install).Name == target.Name) return null;
        if (SessionRuntime.For(target.Identity) == null) return $"{target.Text} can't be played from this launcher: it plays HellForge {SessionRuntime.Supported} and the installed version.";
        if (!File.Exists(Path.Combine(target.App, "session", "HellForge.Session.exe"))) return target.Text + " is not complete (its session runner is missing).";
        var build = Installed(install).GameBuild;
        if (target.GameBuild != null && build != null && target.GameBuild != build)
            return $"{target.Text} plays HELLDIVERS 2 build {target.GameBuild}; this installation's game copy is build {build}.";
        if (DiskSpace.ForFolder(install.Root).FreeBytes is { } free && free < DiskSpace.Margin)
            return $"Not enough free space to switch ({free >> 20} MB free; it needs {DiskSpace.Margin >> 20} MB).";
        return null;
    }

    // Plays with name (a kept version's, or the installed one's) from now on: its app files and its data are swapped in,
    // the version left keeps its own. No program from the installation may run (the caller's own launcher aside).
    public static SwitchResult Activate(InstallLayout install, string name, Func<IReadOnlyList<string>>? blockers = null, Action<string>? step = null)
    {
        Recover(install);
        var installed = Installed(install);
        var active = Active(install);
        var toInstalled = name == installed.Name || name == installed.Text;
        var target = toInstalled ? null : Find(install, name);
        var targetText = target?.Text ?? installed.Text;
        if ((target?.Name ?? installed.Name) == active.Name) return new(active.Text, active.Text, null, []);
        if (target != null && RuntimeProblem(install, target) is { } problem) throw new InvalidDataException(problem);
        if ((blockers ?? (() => Blockers(install)))() is { Count: > 0 } running)
            throw new InvalidDataException("Close these first: " + string.Join(", ", running) + ".");
        PutGameBack(install);
        var root = Root(install);
        var journal = new List<Move>();
        var journalFile = Path.Combine(root, JournalName);
        var staging = Path.Combine(root, IncomingPrefix + "installed");
        var leaving = Path.Combine(root, IncomingPrefix + "left");
        void Rename(string label, string from, string to)
        {
            step?.Invoke(label);
            journal.Add(new(from, to));
            File.WriteAllText(journalFile, JsonSerializer.Serialize(journal, CopyPreparer.JsonOptions));
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            if (Directory.Exists(from)) Directory.Move(from, to);
            else File.Move(from, to);
        }
        IReadOnlyList<string>? seen;
        try
        {
            foreach (var half in new[] { staging, leaving }) if (Directory.Exists(half)) DeleteKept(install, half);
            Directory.CreateDirectory(root);
            LocalInstall.NoLinks(root);
            seen = PlaysKept(install) ? BackToInstalled(install, active, Rename, step) : null;
            if (target != null) seen = PlayKept(install, target, staging, Rename, step);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            var failure = PutBack(install, journal, journalFile, staging, error, active, targetText);
            // all put back: what is left there is the record that was not used
            if (failure is InvalidDataException) TryDeleteKept(install, leaving);
            throw failure;
        }
        File.Delete(journalFile);
        TryDeleteKept(install, leaving);
        var switchedOff = AfterActivate(install, seen, targetText);
        Log(install, $"Plays with {targetText} now (was {active.Text}); the launcher stays {installed.Text}'s." +
            (switchedOff.Count > 0 ? " Off in its mod list, added since it last ran: " + string.Join(", ", switchedOff) + "." : ""));
        return new(active.Text, targetText, null, switchedOff);
    }

    // The version gone to: mods added since it last ran off in its list (returned), and its own bundled mods in place.
    static IReadOnlyList<string> AfterActivate(InstallLayout install, IReadOnlyList<string>? seen, string targetText)
    {
        IReadOnlyList<string> switchedOff = [];
        try { switchedOff = SwitchOffNew(install, seen); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            Log(install, "Mods added since " + targetText + " last ran could not be switched off in its list: " + error.Message);
        }
        // the version now played with ships its own bundled mods (the in-game editor, Hell Forge, Vanilla Plus)
        try { foreach (var line in BundledMods.Sync(install)) Log(install, line); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { Log(install, "Bundled mods not synced: " + error.Message); }
        return switchedOff;
    }

    // The game's files as HellForge found them (the stand-ins, the staged mod files), so the version going in stages its own.
    static void PutGameBack(InstallLayout install)
    {
        try { SdkShimRecovery.Recover(install); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { Log(install, "SDK stand-ins not checked: " + error.Message); }
        ModStaging.Unstage(install);
    }

    // The entries of an app folder a switch moves: all but the launcher.
    static IEnumerable<string> Moved(string app) => Directory.Exists(app)
        ? Directory.EnumerateFileSystemEntries(app).Select(Path.GetFileName).OfType<string>().Where(n => !Stays.Contains(n, StringComparer.OrdinalIgnoreCase)).Order(StringComparer.Ordinal).ToList()
        : [];

    // The kept version played with goes back to its folder with its data; the installed one's come back. Returns the mods the
    // installed version saw when it was left.
    static IReadOnlyList<string>? BackToInstalled(InstallLayout install, AppIdentity active, Action<string, string, string> rename, Action<string>? step)
    {
        var parked = InstalledPath(install);
        var kept = Path.Combine(Root(install), active.Name);
        if (!File.Exists(Path.Combine(kept, RecordName))) throw new InvalidDataException($"{active.Text} is played with, but app-versions has no record of it.");
        foreach (var entry in Moved(install.App)) rename("app", Path.Combine(install.App, entry), Path.Combine(kept, "app", entry));
        foreach (var entry in Moved(Path.Combine(parked, "app"))) rename("app", Path.Combine(parked, "app", entry), Path.Combine(install.App, entry));
        var moved = MoveData(install, Path.Combine(kept, "data"), Path.Combine(parked, "data"), rename);
        step?.Invoke("record");
        var record = Path.Combine(kept, RecordName);
        var fresh = Path.Combine(Root(install), IncomingPrefix + "left", RecordName + ".new");
        Directory.CreateDirectory(Path.GetDirectoryName(fresh)!);
        WriteRecord(fresh, install, active, moved, keptUtc: ReadKeptUtc(record));
        rename("record", record, Path.Combine(Root(install), IncomingPrefix + "left", RecordName + ".old"));
        rename("record", fresh, record);
        IReadOnlyList<string>? seen = null;
        try { seen = JsonSerializer.Deserialize<Left>(File.ReadAllText(Path.Combine(parked, LeftName)), CopyPreparer.JsonOptions)?.Mods; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
        rename("kept", parked, Path.Combine(Root(install), IncomingPrefix + "left", InstalledFolder));
        return seen;
    }

    // The installed version's files and data go to app-versions/.installed; the kept one's come in. Returns the mods the
    // kept version saw when it last ran.
    static IReadOnlyList<string>? PlayKept(InstallLayout install, KeptVersion target, string staging, Action<string, string, string> rename, Action<string>? step)
    {
        LocalInstall.NoLinks(target.Folder);
        Directory.CreateDirectory(staging);
        foreach (var entry in Moved(install.App)) rename("app", Path.Combine(install.App, entry), Path.Combine(staging, "app", entry));
        foreach (var entry in Moved(target.App)) rename("app", Path.Combine(target.App, entry), Path.Combine(install.App, entry));
        MoveData(install, Path.Combine(staging, "data"), Path.Combine(target.Folder, "data"), rename);
        step?.Invoke("record");
        File.WriteAllText(Path.Combine(staging, LeftName), JsonSerializer.Serialize(new Left(ModEntries(install), DateTime.UtcNow, target.Version), CopyPreparer.JsonOptions));
        rename("kept", staging, InstalledPath(install));
        return target.Mods;
    }

    // Each data entry out to outFolder, then the one in inFolder in its place; the entries moved out.
    static List<string> MoveData(InstallLayout install, string outFolder, string inFolder, Action<string, string, string> rename)
    {
        var moved = new List<string>();
        foreach (var entry in DataEntries)
        {
            var live = Entry(install.Root, entry);
            if (File.Exists(live) || Directory.Exists(live))
            {
                rename("data", live, Entry(outFolder, entry));
                moved.Add(entry);
            }
            var incoming = Entry(inFolder, entry);
            if (File.Exists(incoming) || Directory.Exists(incoming)) rename("data", incoming, live);
        }
        return moved;
    }

    static DateTime? ReadKeptUtc(string record)
    {
        try { return JsonSerializer.Deserialize<Record>(File.ReadAllText(record), CopyPreparer.JsonOptions)?.KeptUtc; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // Before an update: the installed version is played with again, and an installation an earlier switch left with an
    // older app in app/ gets its newest app back (RepairSwapped). Throws when either cannot be done: the update must not
    // replace app/ then. Returns what was done, or null.
    public static string? Normalize(InstallLayout install)
    {
        Recover(install);
        string? said = null;
        if (PlaysKept(install))
        {
            var done = Activate(install, Installed(install).Name);
            said = $"Plays with {done.To} again before the update (it played with {done.From}).";
        }
        return RepairSwapped(install) is { } repaired ? (said == null ? repaired : said + " " + repaired) : said;
    }

    // An earlier switch swapped the whole app, launcher and all, so an older app can be in app/ with a newer one kept:
    // the newest kept app goes back in app/ with its data, and the one in app/ is kept again. Returns what was done, or
    // null when nothing was needed.
    public static string? RepairSwapped(InstallLayout install)
    {
        Recover(install);
        if (PlaysKept(install) || !File.Exists(Path.Combine(install.App, "app-build.json"))) return null;
        var app = Identify(install.App);
        KeptVersion? newest = null;
        foreach (var kept in List(install))
            if (kept.Identity.IsNewerThan(app) && File.Exists(Path.Combine(kept.App, "launcher", "HellForge.Launcher.exe")) && (newest == null || kept.Identity.IsNewerThan(newest.Identity)))
                newest = kept;
        if (newest == null) return null;
        Switch(install, newest.Name);
        var repaired = $"{app.Text} was in app/ from an earlier switch; {newest.Text}, the newest version kept here, is back in app/ with its data.";
        Log(install, repaired);
        return repaired;
    }

    // A switch as Setup and HellForge.Cli make it (no launcher open): the newest app back first if needed, then name is
    // played with.
    public static SwitchResult SwitchRuntime(InstallLayout install, string name, Func<IReadOnlyList<string>>? blockers = null)
    {
        var before = Directory.Exists(install.App) ? Active(install).Text : "";
        RepairSwapped(install);
        var done = Activate(install, name, blockers);
        return done with { From = before.Length > 0 ? before : done.From };
    }

    public sealed record SwitchTarget(string Name, string Text);

    // What Setup and HellForge.Cli can switch to: the installed version while a kept one is played with, and each kept one
    // that can be played; after an earlier whole-app switch, also the newer kept app that comes back first.
    public static IReadOnlyList<SwitchTarget> SwitchTargets(InstallLayout install)
    {
        if (!Directory.Exists(install.App)) return [];
        var targets = new List<SwitchTarget>();
        var active = Active(install);
        if (PlaysKept(install)) targets.Add(new(Installed(install).Name, Installed(install).Text));
        foreach (var kept in List(install).Where(k => k.Name != active.Name))
        {
            var comesBack = !PlaysKept(install) && kept.Identity.IsNewerThan(active) && File.Exists(Path.Combine(kept.App, "launcher", "HellForge.Launcher.exe"));
            if (comesBack || RuntimeProblem(install, kept) == null) targets.Add(new(kept.Name, kept.Text));
        }
        return targets;
    }

    // Setup's and HellForge.Cli's outcome for the launcher to show when it opens (TakeResult).
    public static SwitchOutcome Outcome(SwitchResult done) => new("done", done.From, done.To,
        $"Plays with HellForge {done.To} now (it played with {done.From})." + (done.SwitchedOff.Count > 0 ? " Mods added since it last ran are off in its list: " + string.Join(", ", done.SwitchedOff) + "." : ""),
        DateTime.UtcNow);
}
