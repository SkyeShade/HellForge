using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace HellForge.Core;

// The app a kept or running version is: its app-build.json's version and commit, and the engine and game build in engine.json.
public sealed record AppIdentity(string Version, string Commit, string? Engine, string? GameBuild, DateTime? BuiltUtc = null)
{
    public string Name => KeptVersions.FolderNameOf(Version, Commit);
    public string Text => AppVersion.FromParts(Version, Commit);

    // Newer by version number, and for the same number (two builds of one release) by build time.
    public bool IsNewerThan(AppIdentity other)
    {
        var (mine, theirs) = (LauncherUpdate.Number.Parse(Version), LauncherUpdate.Number.Parse(other.Version));
        var order = mine != null && theirs != null ? mine.CompareTo(theirs) : 0;
        return order != 0 ? order > 0 : BuiltUtc != null && other.BuiltUtc != null && BuiltUtc > other.BuiltUtc;
    }
}

// One kept version: <installation>/app-versions/<name> with its app/, its HellForge.exe and data/, the user data it last
// used (KeptVersions.DataEntries).
public sealed record KeptVersion(string Name, string Folder, string Version, string Commit, string? Engine, string? GameBuild,
    DateTime KeptUtc, IReadOnlyList<string> Data, DateTime? BuiltUtc = null, IReadOnlyList<string>? Mods = null)
{
    public string Text => AppVersion.FromParts(Version, Commit);
    public AppIdentity Identity => new(Version, Commit, Engine, GameBuild, BuiltUtc);
    public string App => Path.Combine(Folder, "app");
}

public sealed record KeepResult(bool Kept, string? Name, string Message, IReadOnlyList<string> Dropped);

public sealed record SwitchResult(string From, string To, string? GameBuildNote, IReadOnlyList<string> SwitchedOff);

// Earlier versions kept in the installation, so a player can go back one after an update and forward again. An update keeps
// the app it replaces, the HellForge.exe beside it and a copy of the user data newer versions change one way (snapshot seals,
// settings an older version would drop on save, the assembled catalogue). Going back swaps them in by renames, so the
// version it leaves is kept the same way. The game copy, mods, logs and tools stay shared. Every rename is journalled
// first: a failure, or a kill (Recover), puts each one back. Deletes happen only to direct children of app-versions.
public static partial class KeptVersions
{
    public const string FolderName = "app-versions";
    // How many earlier versions updates keep: the player's choice in Settings, in app-versions/keep.json beside them (not
    // in a version's data, which a switch swaps).
    public const int DefaultCount = 2, MinCount = 1, MaxCount = 5;
    const string KeepFile = "keep.json";
    public const string RecordName = "kept-version.json";
    public const string LogName = "versions.log";
    public const string ResultName = "version-switch.json";
    // Left free after keeping: the game's shader cache and logs still need room.
    public const long Reserve = 2L << 30;
    const string Starter = "HellForge.exe";
    const string IncomingPrefix = ".incoming-";
    const string JournalName = ".switch-journal.json";
    const string SwitcherFolder = "version-switch";

    // The user data each version reads in its own format, relative to the installation ('/').
    public static readonly string[] DataEntries = ["launcher-settings.json", "profiles", "account-snapshot", "catalogue", "mods/hellforge-mods.json"];
    // A Windows installation never runs these; a kept copy goes without them (about half the app).
    static readonly string[] LinuxFolders = ["launcher-linux", "cli-linux", "backend-linux", "capture-linux"];

    [GeneratedRegex("[^0-9A-Za-z._-]")]
    private static partial Regex Unsafe();

    // "0.5.0-beta-19626f6": safe as one folder name whatever app-build.json says.
    public static string FolderNameOf(string version, string commit)
    {
        var name = Unsafe().Replace(commit.Length > 0 ? $"{version}-{commit[..Math.Min(8, commit.Length)]}" : version, "_").TrimStart('.');
        if (name.Length > 64) name = name[..64];
        return name.Length == 0 || name.All(c => c == '.') ? "unknown" : name;
    }

    public static string Root(InstallLayout install) => Path.Combine(install.Root, FolderName);

    public static int KeepCount(InstallLayout install)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(Root(install), KeepFile)));
            return document.RootElement.TryGetProperty("keep", out var keep) && keep.TryGetInt32(out var count) ? Math.Clamp(count, MinCount, MaxCount) : DefaultCount;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { return DefaultCount; }
    }

    // A lower number drops nothing now: the next update keeps that many.
    public static void SetKeepCount(InstallLayout install, int count)
    {
        if (count is < MinCount or > MaxCount) throw new ArgumentException($"Keep {MinCount} to {MaxCount} earlier versions.");
        Directory.CreateDirectory(Root(install));
        LocalInstall.NoLinks(Root(install));
        File.WriteAllText(Path.Combine(Root(install), KeepFile), JsonSerializer.Serialize(new { keep = count }));
    }

    // The space the kept versions take now.
    public static long KeptBytes(InstallLayout install) => List(install).Sum(k => FolderBytes(k.Folder));
    public static string LogFile(InstallLayout install) => Path.Combine(install.Logs, LogName);
    public static string ResultFile(InstallLayout install) => Path.Combine(install.Logs, ResultName);

    public static AppIdentity Identify(string app)
    {
        static JsonElement? Read(string file)
        {
            try { using var document = JsonDocument.Parse(File.ReadAllText(file)); return document.RootElement.Clone(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
        }
        static string? Text(JsonElement? element, string name) =>
            element is { ValueKind: JsonValueKind.Object } e && e.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
        var build = Read(Path.Combine(app, "app-build.json"));
        var engine = Read(Path.Combine(app, "engine.json"));
        return new(Text(build, "version") ?? "unknown", Text(build, "commit") ?? "", Text(engine, "version"), Text(engine, "build"),
            DateTime.TryParse(Text(build, "builtUtc"), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var built) ? built : null);
    }

    // The kept versions, the most recently kept first. A folder without a readable record is not one.
    public static IReadOnlyList<KeptVersion> List(InstallLayout install)
    {
        var root = Root(install);
        if (!Directory.Exists(root)) return [];
        var kept = new List<KeptVersion>();
        foreach (var folder in Directory.GetDirectories(root))
        {
            var name = Path.GetFileName(folder);
            if (name.StartsWith('.') || !File.Exists(Path.Combine(folder, RecordName))) continue;
            try
            {
                var record = JsonSerializer.Deserialize<Record>(File.ReadAllText(Path.Combine(folder, RecordName)), CopyPreparer.JsonOptions);
                if (record?.Version == null) continue;
                kept.Add(new(name, folder, record.Version, record.Commit ?? "", record.Engine, record.GameBuild, record.KeptUtc, record.Data ?? [], record.BuiltUtc, record.Mods));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
        }
        return kept.OrderByDescending(k => k.KeptUtc).ToList();
    }

    // Mods: the mods folder's entries when the version was kept, so mods added since are known as new to it.
    sealed record Record(int Schema, string Version, string? Commit, string? Engine, string? GameBuild, DateTime KeptUtc, List<string>? Data, DateTime? BuiltUtc,
        List<string>? Mods);

    static void WriteRecord(string folder, InstallLayout install, AppIdentity app, IEnumerable<string> data) =>
        File.WriteAllText(Path.Combine(folder, RecordName), JsonSerializer.Serialize(
            new Record(1, app.Version, app.Commit, app.Engine, app.GameBuild, DateTime.UtcNow, data.ToList(), app.BuiltUtc, ModEntries(install)), CopyPreparer.JsonOptions));

    static List<string> ModEntries(InstallLayout install) => !Directory.Exists(install.Mods) ? [] :
        Directory.EnumerateFileSystemEntries(install.Mods).Select(Path.GetFileName).OfType<string>()
            .Where(name => !name.StartsWith('.') && name != ModSettings.FileName && name != ModSettings.LegacyFileName).Order(StringComparer.Ordinal).ToList();

    // Each version has its own mod list (mods/hellforge-mods.json, swapped with its data); the mods themselves are shared.
    // A mod added to the mods folder since the version going in last ran is switched off in its list, so a mod turned on
    // in another version is never on here unless the player turns it on. Returns the ids switched off.
    static IReadOnlyList<string> SwitchOffNew(InstallLayout install, IReadOnlyList<string>? seen)
    {
        if (seen == null) return [];
        var known = new HashSet<string>(seen, StringComparer.OrdinalIgnoreCase);
        // the mods this version ships itself are its own, wherever they first appeared
        if (Directory.Exists(BundledMods.Folder(install))) known.UnionWith(Directory.GetDirectories(BundledMods.Folder(install)).Select(Path.GetFileName).OfType<string>());
        string Top(ModManifest mod) => mod.Archive ?? Path.GetRelativePath(install.Mods, mod.Folder).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];
        var fresh = ModCatalog.Load(install.Mods).Mods.Where(m => m.Id != ModCatalog.LocalStatsId && !known.Contains(Top(m))).Select(m => m.Id).Distinct().ToList();
        if (fresh.Count == 0) return fresh;
        var path = Path.Combine(install.Mods, ModSettings.FileName);
        var root = File.Exists(path) ? JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new InvalidDataException(path + " is not a JSON object.") : new JsonObject { ["format"] = 1 };
        if (root["disabled"] is not JsonArray disabled) root["disabled"] = disabled = [];
        var off = disabled.Select(d => d?.GetValue<string>()).ToHashSet(StringComparer.Ordinal);
        foreach (var id in fresh.Where(id => !off.Contains(id))) disabled.Add(id);
        File.WriteAllText(path + ".tmp", root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        File.Move(path + ".tmp", path, overwrite: true);
        return fresh;
    }

    public static void Log(InstallLayout install, string line)
    {
        try
        {
            Directory.CreateDirectory(install.Logs);
            File.AppendAllText(LogFile(install), DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss'Z'", CultureInfo.InvariantCulture) + " " + line + Environment.NewLine);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static long FolderBytes(string path) =>
        File.Exists(path) ? new FileInfo(path).Length
        : Directory.Exists(path) ? new DirectoryInfo(path).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length) : 0;

    static string Entry(string root, string entry) => Path.Combine(root, entry.Replace('/', Path.DirectorySeparatorChar));

    public static long DataBytes(InstallLayout install) => DataEntries.Sum(e => FolderBytes(Entry(install.Root, e)));

    // Called by an update before the new app goes in: keeps install.App (moved, so it costs no space) with its HellForge.exe
    // and a copy of the user data, then drops the oldest beyond Count. Never fails the update: when nothing is kept, app/
    // is left for the caller to replace as before, and the message says why. freeBytes: the tests' drive.
    public static KeepResult KeepCurrent(InstallLayout install, string newApp, Func<string, long?>? freeBytes = null, bool freshModList = true)
    {
        try { Recover(install); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return NotKept(install, "an earlier switch could not be put back (" + error.Message + ")");
        }
        // a new installation's app folder is still empty: nothing ran there yet
        if (!File.Exists(Path.Combine(install.App, "app-build.json"))) return new(false, null, "No earlier version to keep.", []);
        var old = Identify(install.App);
        var incoming = Identify(newApp);
        if (old.Name == incoming.Name) return new(false, null, $"{old.Text} is installed again; nothing new to keep.", []);
        var existing = List(install);
        // Setup of a version kept here (from a version without the list, such as 0.4): its own data comes back, as
        // picking it in the list does; Setup then puts its fresh copy of the same app in place.
        if (existing.FirstOrDefault(k => k.Name == incoming.Name) is { } again && Problem(install, again) == null)
            return SwitchForUpdate(install, again, old);
        var replaced = existing.Where(k => k.Name == old.Name || k.Name == incoming.Name).ToList();
        var dropped = existing.Except(replaced).Skip(KeepCount(install) - 1).ToList();
        if (RoomProblem(install, dropped.Concat(replaced), freeBytes) is { } room) return NotKept(install, room);
        if (MoveIntoKept(install, old, replaced) is { } failed) return NotKept(install, failed);
        StripLinux(install, Path.Combine(Root(install), old.Name, "app"));
        if (freshModList) StartModList(install, newApp, old);
        var names = dropped.Where(kept => TryDeleteKept(install, kept.Folder)).Select(kept => kept.Text).ToList();
        var message = $"Kept {old.Text}: the version in the launcher's corner can go back to it." + (names.Count > 0 ? " No longer kept: " + string.Join(", ", names) + "." : "");
        Log(install, message.Replace(": the version in the launcher's corner can go back to it", $" (replaced by {incoming.Text})"));
        return new(true, old.Name, message, names);
    }

    static KeepResult SwitchForUpdate(InstallLayout install, KeptVersion again, AppIdentity old)
    {
        try
        {
            Switch(install, again.Name, () => []);
            var message = $"Kept {old.Text}; {again.Text} was kept here, so its own data and mod list are back.";
            Log(install, message);
            return new(true, old.Name, message, []);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return NotKept(install, $"{again.Text}'s kept data could not be swapped back ({error.Message})");
        }
    }

    // The new version starts with a mod list of its own: every mod off but those it ships, so nothing from the version
    // before is on without the player choosing it. The Mods page offers to import the kept version's list (ImportModList).
    static void StartModList(InstallLayout install, string newApp, AppIdentity old)
    {
        try
        {
            var shipped = BundledNames(newApp);
            var off = ModCatalog.Load(install.Mods).Mods.Where(m => m.Id != ModCatalog.LocalStatsId && !shipped.Contains(TopEntry(install, m))).Select(m => m.Id).ToList();
            ModSettings.StartFresh(install.Mods, off);
            Log(install, $"The new version starts with its own mod list; {off.Count} mods from {old.Text} are off until imported or turned on.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            Log(install, "The new version kept the mod list of the version before (" + error.Message + ").");
        }
    }

    static HashSet<string> BundledNames(string app)
    {
        var folder = Path.Combine(app, BundledMods.FolderName);
        return Directory.Exists(folder) ? Directory.GetDirectories(folder).Select(Path.GetFileName).OfType<string>().ToHashSet(StringComparer.OrdinalIgnoreCase)
            : new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    }

    // The entry in the mods folder a mod came from: its zip, or its first folder.
    static string TopEntry(InstallLayout install, ModManifest mod) =>
        mod.Archive ?? Path.GetRelativePath(install.Mods, mod.Folder).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];

    static bool Knew(InstallLayout install, KeptVersion kept, ModManifest mod) => kept.Mods == null || kept.Mods.Contains(TopEntry(install, mod), StringComparer.OrdinalIgnoreCase);

    static ModSettings.State ListOf(KeptVersion kept) => ModSettings.Read(Path.Combine(kept.Folder, "data", "mods"));

    // The mods a kept version has on, of those in the mods folder now (the versions' own shipped mods and Local Stats aside).
    public static int ModsOn(InstallLayout install, KeptVersion kept, IEnumerable<ModManifest> mods, ISet<string> bundled)
    {
        var list = ListOf(kept);
        return mods.Count(m => m.Id != ModCatalog.LocalStatsId && !bundled.Contains(m.Id) && Knew(install, kept, m) && !list.Disabled.Contains(m.Id));
    }

    // The kept versions whose mod list the Mods page offers to import, the most recent first: while this version has no mod
    // on (its shipped ones aside), the offer was neither taken nor dismissed, and the kept version has mods on.
    public static IReadOnlyList<KeptVersion> ImportSources(InstallLayout install, IReadOnlyList<ModManifest> mods, ModSettings.State current, ISet<string> bundled)
    {
        if (current.ImportDone || mods.Any(m => m.Enabled && m.Id != ModCatalog.LocalStatsId && !bundled.Contains(m.Id))) return [];
        return List(install).Where(k => ModsOn(install, k, mods, bundled) > 0).ToList();
    }

    // A kept version's mod list brought into this one: which mods are on, their order, pack choices and trust.
    public static void ImportModList(InstallLayout install, KeptVersion kept, IEnumerable<ModManifest> mods)
    {
        var ids = mods.Where(m => Knew(install, kept, m)).Select(m => m.Id).ToList();
        ModSettings.Import(install.Mods, ListOf(kept), ids);
        Log(install, $"Imported the mod list of {kept.Text} ({ids.Count} mods it knew).");
    }

    // Why keeping would leave the drive too full (the dropped versions' space counts as freed), or null.
    static string? RoomProblem(InstallLayout install, IEnumerable<KeptVersion> freed, Func<string, long?>? freeBytes)
    {
        var free = (freeBytes ?? (folder => DiskSpace.ForFolder(folder).FreeBytes))(install.Root);
        if (free is not { } bytes) return null;
        return bytes - DataBytes(install) + freed.Sum(k => FolderBytes(k.Folder)) < Reserve
            ? $"only {bytes / 1e9:0.0} GB is free, and keeping it should leave {Reserve / 1e9:0.0} GB" : null;
    }

    // The data copied and the app moved into app-versions/<name>; null, or why not (app/ is then where it was).
    static string? MoveIntoKept(InstallLayout install, AppIdentity old, IReadOnlyList<KeptVersion> replaced)
    {
        var root = Root(install);
        var staging = Path.Combine(root, IncomingPrefix + old.Name);
        var moved = false;
        try
        {
            Directory.CreateDirectory(root);
            LocalInstall.NoLinks(root);
            if (Directory.Exists(staging)) DeleteKept(install, staging);
            var copied = DataEntries.Where(entry => File.Exists(Entry(install.Root, entry)) || Directory.Exists(Entry(install.Root, entry))).ToList();
            foreach (var entry in copied) CopyEntry(Entry(install.Root, entry), Entry(Path.Combine(staging, "data"), entry));
            Directory.CreateDirectory(staging);
            var starter = Path.Combine(install.Root, Starter);
            if (File.Exists(starter)) File.Copy(starter, Path.Combine(staging, Starter));
            WriteRecord(staging, install, old, copied);
            Directory.Move(install.App, Path.Combine(staging, "app"));
            moved = true;
            foreach (var kept in replaced) DeleteKept(install, kept.Folder);
            Directory.Move(staging, Path.Combine(root, old.Name));
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            if (moved && !Directory.Exists(install.App)) Directory.Move(Path.Combine(staging, "app"), install.App);
            TryDeleteKept(install, staging);
            return error.Message;
        }
    }

    static KeepResult NotKept(InstallLayout install, string why)
    {
        var message = "The version before this update was not kept: " + why + ".";
        Log(install, message);
        return new(false, null, message, []);
    }

    // Puts a version KeepCurrent kept back as the app, when the update could not put the new one in.
    public static void Unkeep(InstallLayout install, string name)
    {
        var folder = Path.Combine(Root(install), name);
        if (Directory.Exists(install.App)) throw new IOException(install.App + " exists.");
        Directory.Move(Path.Combine(folder, "app"), install.App);
        var starter = Path.Combine(folder, Starter);
        if (File.Exists(starter)) File.Copy(starter, Path.Combine(install.Root, Starter), overwrite: true);
        TryDeleteKept(install, folder);
        Log(install, $"The update did not finish: {name} is the app again.");
    }

    static void CopyEntry(string from, string to)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        if (File.Exists(from)) { File.Copy(from, to); return; }
        foreach (var file in LocalInstall.FilesNoLinks(from))
        {
            var target = Path.Combine(to, Path.GetRelativePath(from, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target);
        }
        Directory.CreateDirectory(to);
    }

    static void StripLinux(InstallLayout install, string app)
    {
        if (!OperatingSystem.IsWindows() || LinuxNetworkBlock.UnderWine) return;
        foreach (var name in LinuxFolders)
        {
            var folder = Path.Combine(app, name);
            if (!Directory.Exists(folder) || !LocalInstall.Within(folder, Root(install))) continue;
            try { Directory.Delete(folder, recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    // Deletes a kept or half-made version: only a folder directly in app-versions, by a name this class makes, with no
    // link on its way.
    public static void DeleteKept(InstallLayout install, string folder)
    {
        var root = LocalInstall.FullPath(Root(install));
        var full = LocalInstall.FullPath(folder);
        var name = Path.GetFileName(full);
        if (!string.Equals(Path.GetDirectoryName(full), root, StringComparison.OrdinalIgnoreCase) || name.Length == 0 || name is "." or ".." ||
            Unsafe().IsMatch(name))
            throw new InvalidDataException("Refused to delete " + full + ": it is not a kept version in " + root + ".");
        if (!Directory.Exists(full)) return;
        LocalInstall.NoLinks(full);
        Directory.Delete(full, recursive: true);
    }

    static bool TryDeleteKept(InstallLayout install, string folder)
    {
        try { DeleteKept(install, folder); return true; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            Log(install, "Could not delete " + folder + ": " + error.Message);
            return false;
        }
    }

    // What keeps a switch from starting: the game, a session, or any program running from the installation (its launcher
    // too, unless ignored: the one asking waits for it to close).
    public static IReadOnlyList<string> Blockers(InstallLayout install, IEnumerable<int>? ignore = null)
    {
        var skip = new HashSet<int>(ignore ?? []) { Environment.ProcessId };
        var found = new List<string>();
        if (InventorySync.IsGameRunning()) found.Add("HELLDIVERS 2");
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (skip.Contains(process.Id)) continue;
                try
                {
                    if (process.ProcessName == "HellForge.Session" || process.MainModule?.FileName is { } file && LocalInstall.Within(file, install.Root))
                        found.Add(process.ProcessName);
                }
                catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { }
            }
        }
        return found.Distinct().ToList();
    }

    // Why a kept version cannot be switched to, or null.
    public static string? Problem(InstallLayout install, KeptVersion target)
    {
        if (!Directory.Exists(install.App)) return "This installation has no app folder.";
        if (Identify(install.App).Name == target.Name) return target.Text + " is the version installed now.";
        var launcher = OperatingSystem.IsWindows() ? Path.Combine("launcher", "HellForge.Launcher.exe") : Path.Combine("launcher-linux", "HellForge.Launcher");
        if (!File.Exists(Path.Combine(target.App, launcher))) return target.Text + " is not complete (its launcher is missing).";
        if (Directory.Exists(Path.Combine(Root(install), Identify(install.App).Name))) return "A kept folder already has the installed version's name.";
        // renames take no space, but the journal, the record and the next session's staging do
        if (DiskSpace.ForFolder(install.Root).FreeBytes is { } free && free < DiskSpace.Margin)
            return $"Not enough free space to switch ({free >> 20} MB free; it needs {DiskSpace.Margin >> 20} MB).";
        return null;
    }

    public static KeptVersion Find(InstallLayout install, string name) =>
        List(install).FirstOrDefault(k => k.Name == name || k.Text == name) ??
        throw new InvalidDataException($"No kept version {name}. Kept: " + (List(install) is { Count: > 0 } kept ? string.Join(", ", kept.Select(k => k.Name)) : "none") + ".");

    // A note when the version gone to plays another game build than this installation's copy has.
    public static string? GameBuildNote(InstallLayout install, KeptVersion target)
    {
        var current = Identify(install.App).GameBuild;
        return target.GameBuild == null || current == null || target.GameBuild == current ? null
            : $"{target.Text} plays HELLDIVERS 2 build {target.GameBuild}, and this installation's game copy is build {current}: after the switch, its Check and repair copies build {target.GameBuild} again.";
    }

    // Swaps a kept version in. The game's files go back to as HellForge found them first (the SDK stand-ins, this version's
    // staged mod files), so the version going in stages its own. Then each rename is journalled and made; a failure puts
    // every one back and throws. step: called before each rename (the tests' failure point).
    public static SwitchResult Switch(InstallLayout install, string name, Func<IReadOnlyList<string>>? blockers = null, Action<string>? step = null)
    {
        Recover(install);
        var target = Find(install, name);
        if (Problem(install, target) is { } problem) throw new InvalidDataException(problem);
        if ((blockers ?? (() => Blockers(install)))() is { Count: > 0 } running)
            throw new InvalidDataException("Close these first: " + string.Join(", ", running) + ".");
        LocalInstall.NoLinks(target.Folder);
        var current = Identify(install.App);
        var note = GameBuildNote(install, target);
        try { SdkShimRecovery.Recover(install); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { Log(install, "SDK stand-ins not checked: " + error.Message); }
        ModStaging.Unstage(install);

        var root = Root(install);
        var staging = Path.Combine(root, IncomingPrefix + current.Name);
        if (Directory.Exists(staging)) DeleteKept(install, staging);
        var journal = new List<Move>();
        var journalFile = Path.Combine(root, JournalName);
        void Rename(string label, string from, string to)
        {
            step?.Invoke(label);
            journal.Add(new(from, to));
            File.WriteAllText(journalFile, JsonSerializer.Serialize(journal, CopyPreparer.JsonOptions));
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            if (Directory.Exists(from)) Directory.Move(from, to);
            else File.Move(from, to);
        }
        try
        {
            Directory.CreateDirectory(staging);
            SwapIn(install, target, current, staging, Rename, step);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            throw PutBack(install, journal, journalFile, staging, error, current, target);
        }
        File.Delete(journalFile);
        TryDeleteKept(install, target.Folder);
        StripLinux(install, Path.Combine(root, current.Name, "app"));
        IReadOnlyList<string> switchedOff = [];
        try { switchedOff = SwitchOffNew(install, target.Mods); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or JsonException)
        {
            Log(install, "Mods added since " + target.Text + " last ran could not be switched off in its list: " + error.Message);
        }
        Log(install, $"Switched from {current.Text} to {target.Text}; {current.Text} is kept." +
            (switchedOff.Count > 0 ? " Off in its mod list, added since it last ran: " + string.Join(", ", switchedOff) + "." : ""));
        return new(current.Text, target.Text, note, switchedOff);
    }

    // Each rename of a switch, in order: the apps, the starters, then each data entry out and the kept one in.
    static void SwapIn(InstallLayout install, KeptVersion target, AppIdentity current, string staging, Action<string, string, string> rename, Action<string>? step)
    {
        rename("app", install.App, Path.Combine(staging, "app"));
        rename("app", target.App, install.App);
        var starter = Path.Combine(install.Root, Starter);
        if (File.Exists(Path.Combine(target.Folder, Starter)))
        {
            if (File.Exists(starter)) rename("starter", starter, Path.Combine(staging, Starter));
            rename("starter", Path.Combine(target.Folder, Starter), starter);
        }
        var moved = new List<string>();
        foreach (var entry in DataEntries)
        {
            var live = Entry(install.Root, entry);
            if (File.Exists(live) || Directory.Exists(live))
            {
                rename("data", live, Entry(Path.Combine(staging, "data"), entry));
                moved.Add(entry);
            }
            var kept = Entry(Path.Combine(target.Folder, "data"), entry);
            if (File.Exists(kept) || Directory.Exists(kept)) rename("data", kept, live);
        }
        step?.Invoke("record");
        WriteRecord(staging, install, current, moved);
        rename("kept", staging, Path.Combine(Root(install), current.Name));
    }

    // A failed switch's renames undone; the exception to throw says whether that worked.
    static Exception PutBack(InstallLayout install, List<Move> journal, string journalFile, string staging, Exception error, AppIdentity current, KeptVersion target)
    {
        if (Undo(journal) is { } undo)
        {
            Log(install, $"Going to {target.Text} failed ({error.Message}) and could not be put back fully ({undo}); the next update or switch retries from {journalFile}.");
            return new IOException($"{target.Text} could not be swapped in ({error.Message}), and putting {current.Text} back failed too ({undo}). " +
                "Close every program that may hold its files and run the switch again, or run HellForge-Setup over the installation: either finishes putting it back.", error);
        }
        File.Delete(journalFile);
        TryDeleteKept(install, staging);
        Log(install, $"Going to {target.Text} failed ({error.Message}); {current.Text} and its data were put back as they were.");
        return new InvalidDataException($"{target.Text} could not be swapped in ({error.Message}). Nothing changed: {current.Text} is still installed.", error);
    }

    sealed record Move(string From, string To);

    // Each rename undone, the last first; null, or what could not be.
    static string? Undo(List<Move> journal)
    {
        string? failed = null;
        for (var i = journal.Count - 1; i >= 0; i--)
        {
            var (from, to) = (journal[i].From, journal[i].To);
            try
            {
                if (Directory.Exists(to) && !Directory.Exists(from)) Directory.Move(to, from);
                else if (File.Exists(to) && !File.Exists(from)) File.Move(to, from);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { failed ??= $"{to} -> {from}: {error.Message}"; }
        }
        return failed;
    }

    // A switch a kill cut short: its journal's renames undone.
    public static void Recover(InstallLayout install)
    {
        var journalFile = Path.Combine(Root(install), JournalName);
        if (!File.Exists(journalFile)) return;
        List<Move> journal;
        try { journal = JsonSerializer.Deserialize<List<Move>>(File.ReadAllText(journalFile), CopyPreparer.JsonOptions) ?? []; }
        catch (JsonException) { journal = []; }
        foreach (var move in journal)
            if (!LocalInstall.Within(move.From, install.Root) || !LocalInstall.Within(move.To, install.Root))
                throw new InvalidDataException("The switch journal names a path outside the installation: " + journalFile);
        if (Undo(journal) is { } failed) throw new IOException("An earlier switch could not be put back: " + failed);
        File.Delete(journalFile);
        foreach (var half in Directory.GetDirectories(Root(install), IncomingPrefix + "*")) TryDeleteKept(install, half);
        Log(install, "An earlier switch that did not finish was put back.");
    }

    // A copy of the running HellForge.Cli outside app/, which a switch moves: install/temp/version-switch/cli-<id>. Earlier
    // copies no program runs any more go first.
    public static string SwitcherCopy(InstallLayout install, string cliFolder)
    {
        var parent = Path.Combine(install.Temp, SwitcherFolder);
        Directory.CreateDirectory(parent);
        LocalInstall.NoLinks(parent);
        foreach (var old in Directory.GetDirectories(parent, "cli-*"))
            try { Directory.Delete(old, recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var copy = Path.Combine(parent, "cli-" + Guid.NewGuid().ToString("N")[..12]);
        CopyEntry(LocalInstall.FullPath(cliFolder), copy);
        return copy;
    }

    public static bool IsSwitcherCopy(InstallLayout install, string folder) => LocalInstall.Within(folder, Path.Combine(install.Temp, SwitcherFolder));

    public sealed record SwitchOutcome(string Status, string? From, string To, string Message, DateTime Utc);

    public static void WriteResult(InstallLayout install, SwitchOutcome outcome)
    {
        Directory.CreateDirectory(install.Logs);
        var file = ResultFile(install);
        File.WriteAllText(file + ".partial", JsonSerializer.Serialize(outcome, CopyPreparer.JsonOptions));
        File.Move(file + ".partial", file, overwrite: true);
    }

    // The last switch's outcome, once: the launcher shows it when it starts again.
    public static SwitchOutcome? TakeResult(InstallLayout install)
    {
        var file = ResultFile(install);
        try
        {
            if (!File.Exists(file)) return null;
            var outcome = JsonSerializer.Deserialize<SwitchOutcome>(File.ReadAllText(file), CopyPreparer.JsonOptions);
            File.Delete(file);
            return outcome;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }
}
