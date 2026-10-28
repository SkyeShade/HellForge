using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core;

public enum LinuxUninstallKind { File, Directory, EmptyDirectory }

// One path from an uninstall plan: what it is, whether it is a file or a folder, and for files whose content proves they
// belong to this installation, the SHA-256 they had when the confirmation was shown.
public sealed record LinuxUninstallItem(string Path, LinuxUninstallKind Kind, string Description,
    string? Root = null, string? Sha256 = null);

public sealed record LinuxUninstallSteam(SupportedBuild Build, string Root,
    IReadOnlyList<LinuxUninstallItem> Depots, IReadOnlyList<LinuxUninstallItem> AppFolders,
    IReadOnlyList<LinuxUninstallItem> Patches);

// What Settings > Uninstall removes outside the installation now, and what stays for the player to remove or keep. Steam's
// leftovers and the removed shell installer's files are removed before the launcher exits; the installation root and the
// desktop shortcut are left to the helper that runs afterwards, and AppArmor is only ever removed by the player with sudo.
public sealed record LinuxUninstallPlan(
    string Root, string? Prefix, LinuxUninstallSteam? Steam,
    IReadOnlyList<LinuxUninstallItem> OldScript, IReadOnlyList<string> Left,
    IReadOnlyList<string> AppArmorPaths, IReadOnlyList<string> AppArmorCommands, string LogPath)
{
    public string? Shortcut { get; init; }
    public string? ShortcutExec { get; init; }
    public bool ShortcutRemovable { get; init; }
    // The prefix deletion offer: only a prefix this installer made, still containing this installation, and not a link,
    // in use, or in a forbidden place is offered.
    public LinuxPrefixOffer? PrefixDeletion { get; init; }
}

public sealed record LinuxUninstallResult(IReadOnlyList<string> Removed, IReadOnlyList<string> Failed, IReadOnlyList<string> Left);

public sealed record LinuxUninstallLocations(string Home, string? ConfigHome = null, string? DataHome = null);

// What the disk says about a prefix before uninstall: whether it is there, what its marker is and says, whether a link is
// on the path to it or below it, and whether a process is using it.
public sealed record LinuxPrefixObservation(bool Exists, bool MarkerExists, bool MarkerPlainFile, string? MarkerText,
    bool PrefixLink, bool LinksBelow, bool InUse);

// Whether uninstall may offer to delete a prefix, and why not. Only a prefix this installer made (its marker names it) and
// that still contains this installation is offered; home, /, Steam's folder, Steam's compatibility data, links and a prefix
// in use are refused.
public sealed record LinuxPrefixOffer(string Prefix, bool Offered, string Reason);

// The paths HellForge and the removed shell installer put outside an installation on Linux, and the decisions that say
// whether they belong to this installation. The path checks are pure so they can be tested without a real Steam or prefix;
// the plan and Remove read the disk and report every failure without stopping the other removals.
public static class LinuxUninstall
{
    public const string AppArmorProfile = "/etc/apparmor.d/hellforge-unshare";
    public const string AppArmorUnshare = "/usr/local/lib/hellforge/unshare";
    public const string AppArmorDirectory = "/usr/local/lib/hellforge";
    public const string OldConfigHeader = "# written by hellforge-linux-install.sh";
    public const string OldScriptHeader =
        "# hellforge-linux-install.sh: guided HellForge setup on Linux (Proton). Experimental helper.";

    public static string OldConfigPath(LinuxUninstallLocations locations) =>
        Path.Combine(string.IsNullOrWhiteSpace(locations.ConfigHome)
            ? Path.Combine(locations.Home, ".config") : locations.ConfigHome, "hellforge-linux", "config");

    public static string OldCommandPath(LinuxUninstallLocations locations) =>
        Path.Combine(locations.Home, ".local", "bin", "hellforge-linux");

    public static string OldMenuEntryPath(LinuxUninstallLocations locations) =>
        Path.Combine(string.IsNullOrWhiteSpace(locations.DataHome)
            ? Path.Combine(locations.Home, ".local", "share") : locations.DataHome, "applications", "hellforge-linux.desktop");

    // The old script's settings name the prefix it served; they are this installation's only when that is its prefix.
    public static bool OldConfigBelongs(string text, string prefix)
    {
        var config = Normalize(text);
        if (!config.Equals(OldConfigHeader, StringComparison.Ordinal) &&
            !config.StartsWith(OldConfigHeader + "\n", StringComparison.Ordinal)) return false;
        var line = config.Split('\n').FirstOrDefault(value => value.StartsWith("PFX=", StringComparison.Ordinal));
        if (line == null) return false;
        try { return LocalInstall.FullPath(Unquote(line[4..])).Equals(LocalInstall.FullPath(prefix), StringComparison.Ordinal); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    // The ~/.local/bin/hellforge-linux copy is the old script itself: its first two lines identify it.
    public static bool OldCommandBelongs(string text) =>
        Normalize(text).StartsWith("#!/usr/bin/env bash\n" + OldScriptHeader + "\n", StringComparison.Ordinal);

    // The old app-menu entry starts that command and carries the old script's name.
    public static bool OldMenuEntryBelongs(string text, string commandPath)
    {
        var menu = Normalize(text);
        var exec = "Exec=\"" + commandPath + "\" run";
        return menu.Contains("Name=HellForge (Linux)", StringComparison.Ordinal) &&
            menu.Split('\n').Any(line => line.TrimEnd() == exec);
    }

    public static IReadOnlyList<string> AppArmorPaths(bool profile, bool copy) =>
        [.. new[] { profile ? AppArmorProfile : null, copy ? AppArmorUnshare : null }.OfType<string>()];

    // Plain commands the player can paste after the installation is gone. The launcher cannot run sudo.
    public static IReadOnlyList<string> AppArmorCommands(bool profile, bool copy)
    {
        var commands = new List<string>();
        if (profile) commands.Add("sudo apparmor_parser -R " + AppArmorProfile + " 2>/dev/null || true");
        var paths = AppArmorPaths(profile, copy);
        if (paths.Count > 0) commands.Add("sudo rm -f -- " + string.Join(' ', paths));
        if (copy) commands.Add("sudo rmdir " + AppArmorDirectory + " 2>/dev/null || true");
        return commands;
    }

    // A prefix gets this marker only when the native installer made the folder (it did not exist, or was empty, before
    // InitializePrefix). The marker holds the prefix's full path, so uninstall can tell it is the same prefix and not one
    // HELLDIVERS 2 or the player chose.
    public const string PrefixMarkerFile = ".hellforge-created";

    public static bool PrefixWasCreated(bool existed, bool empty) => !existed || empty;

    public static string PrefixMarkerPath(string prefix) => Path.Combine(LocalInstall.FullPath(prefix), PrefixMarkerFile);

    public static string PrefixMarkerText(string prefix) => LocalInstall.FullPath(prefix);

    public static bool PrefixMarkerBelongs(string text, string prefix)
    {
        try { return text.TrimEnd('\r', '\n').Equals(LocalInstall.FullPath(prefix), StringComparison.Ordinal); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    // The installation must be under <prefix>/pfx/drive_c: a marker alone must not let uninstall delete a prefix that no
    // longer holds the installation it was made for.
    public static bool PrefixContainsInstall(string prefix, string installRoot)
    {
        try
        {
            var driveC = Path.Combine(LocalInstall.FullPath(prefix), "pfx", "drive_c");
            return LocalInstall.Within(installRoot, driveC);
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    // Paths that are never offered, even with a matching marker: the player's home, the filesystem root, Steam's folder and
    // Steam's compatibility data, where HELLDIVERS 2's own prefix lives. The compatdata check is by path component so it
    // also covers Steam libraries other than the one SteamLocator found.
    public static bool PrefixForbidden(string prefix, string home, string? steamRoot)
    {
        try
        {
            var path = LocalInstall.FullPath(prefix);
            var pathRoot = Path.GetPathRoot(path);
            if (pathRoot != null && path.Equals(LocalInstall.FullPath(pathRoot), StringComparison.Ordinal)) return true;
            if (!string.IsNullOrWhiteSpace(home) &&
                path.Equals(LocalInstall.FullPath(home), StringComparison.Ordinal)) return true;
            if (UnderCompatData(path)) return true;
            if (string.IsNullOrWhiteSpace(steamRoot)) return false;
            return path.Equals(LocalInstall.FullPath(steamRoot), StringComparison.Ordinal);
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return true; }
    }

    static bool UnderCompatData(string path)
    {
        var parts = path.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries);
        for (var index = 0; index + 1 < parts.Length; index++)
            if (parts[index].Equals("steamapps", StringComparison.OrdinalIgnoreCase) &&
                parts[index + 1].Equals("compatdata", StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    // The decision is pure: the disk checks below fill LinuxPrefixObservation, and tests can build one directly.
    public static LinuxPrefixOffer DecidePrefixOffer(string prefix, string installRoot, string home, string? steamRoot,
        LinuxPrefixObservation observation)
    {
        if (PrefixForbidden(prefix, home, steamRoot))
            return new(prefix, false, "it is the home folder, the filesystem root, Steam's folder or under Steam's compatibility data");
        if (!observation.Exists) return new(prefix, false, "the prefix folder does not exist");
        if (observation.PrefixLink) return new(prefix, false, "a link is on the path to it or it is a link");
        if (!observation.MarkerExists)
            return new(prefix, false, "it has no HellForge marker (prefixes made by earlier HellForge versions have none)");
        if (!observation.MarkerPlainFile) return new(prefix, false, "its HellForge marker is not a plain file");
        if (observation.MarkerText == null) return new(prefix, false, "its HellForge marker could not be read");
        if (!PrefixMarkerBelongs(observation.MarkerText, prefix))
            return new(prefix, false, "its HellForge marker names another prefix");
        if (!PrefixContainsInstall(prefix, installRoot))
            return new(prefix, false, "it does not contain this installation");
        if (observation.LinksBelow) return new(prefix, false, "it contains a link");
        if (observation.InUse)
            return new(prefix, false, "a process is using it (WINEPREFIX points at its pfx folder, or its executable or working folder is inside it)");
        return new(prefix, true, "");
    }

    // Reads the disk and decides whether the confirmation may offer the prefix. The caller repeats this after the player
    // confirms, because a process can start or the marker can change in between.
    public static LinuxPrefixOffer CheckPrefixDeletion(InstallLayout install, string prefix,
        LinuxUninstallLocations locations, string? steamRoot)
    {
        prefix = LocalInstall.FullPath(prefix);
        var markerPath = PrefixMarkerPath(prefix);
        var exists = Directory.Exists(prefix) || File.Exists(prefix);
        var prefixLink = exists && HasLink(prefix);
        var markerExists = Exists(markerPath);
        var markerPlain = false;
        string? markerText = null;
        if (markerExists && !IsLink(markerPath))
        {
            try
            {
                LocalInstall.NoLinks(markerPath);
                if (File.Exists(markerPath)) { markerText = File.ReadAllText(markerPath); markerPlain = true; }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        }
        var candidate = exists && !prefixLink && markerExists && markerPlain && markerText != null &&
            PrefixMarkerBelongs(markerText, prefix) && PrefixContainsInstall(prefix, install.Root) &&
            !PrefixForbidden(prefix, locations.Home, steamRoot);
        var linksBelow = candidate && HasLinkBelow(prefix);
        var inUse = candidate && LinuxInstallProcesses.RunningInPrefix(prefix).Count > 0;
        return DecidePrefixOffer(prefix, install.Root, locations.Home, steamRoot,
            new(exists, markerExists, markerPlain, markerText, prefixLink, linksBelow, inUse));
    }

    // Writes the marker only for a prefix this installer made; an existing non-empty folder (a shared or HELLDIVERS 2
    // prefix) never gets one.
    public static bool WritePrefixMarker(string prefix, bool existed, bool empty)
    {
        if (!PrefixWasCreated(existed, empty)) return false;
        File.WriteAllText(PrefixMarkerPath(prefix), PrefixMarkerText(prefix) + "\n");
        return true;
    }

    static bool HasLink(string path)
    {
        try { LocalInstall.NoLinks(path); return false; }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return true; }
    }

    // The same rule as NoLinksBelow, with one exception: Wine's own pfx/dosdevices drive letters are links by design and
    // are not followed. Every other link under the prefix refuses the deletion.
    static bool HasLinkBelow(string prefix)
    {
        try { return HasLinkBelow(new DirectoryInfo(prefix), Path.Combine(prefix, "pfx", "dosdevices")); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return true; }
    }

    static bool HasLinkBelow(DirectoryInfo folder, string dosdevices)
    {
        foreach (var entry in folder.EnumerateFileSystemInfos())
        {
            if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) return true;
            if ((entry.Attributes & FileAttributes.Directory) == 0) continue;
            if (entry.FullName.Equals(dosdevices, StringComparison.Ordinal)) continue;
            if (HasLinkBelow(new DirectoryInfo(entry.FullName), dosdevices)) return true;
        }
        return false;
    }

    // Steam's state files are named state_<app>_<something>.patch (the old script's state_553850_*.patch).
    public static bool StatePatchName(string path, string appId)
    {
        var name = Path.GetFileName(path);
        return name.StartsWith("state_" + appId + "_", StringComparison.Ordinal) &&
            name.EndsWith(".patch", StringComparison.Ordinal);
    }

    // A removal stays inside the Steam folder it was found in.
    public static bool Within(string root, string path)
    {
        try { return LocalInstall.Within(path, root); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) { return false; }
    }

    public static bool Exists(string path)
    {
        try { File.GetAttributes(path); return true; }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException or IOException or UnauthorizedAccessException) { return false; }
    }

    // Builds the list the confirmation shows and the removal uses. A path that is a link, is outside its folder, or cannot
    // be read is left and named, not followed. Steam is searched only in its four content folders and its steamapps trees.
    public static LinuxUninstallPlan Plan(InstallLayout install, SupportedBuild build, LinuxUninstallLocations locations,
        string? steamRoot, string logPath)
    {
        var left = new List<string>();
        var prefix = Prefix(install.Root, left);
        var oldScript = OldScriptFiles(prefix, locations, left);
        var steam = SteamFiles(steamRoot, build, left);
        var profile = Exists(AppArmorProfile);
        var copy = Exists(AppArmorUnshare);
        return new(install.Root, prefix, steam, oldScript, left, AppArmorPaths(profile, copy),
            AppArmorCommands(profile, copy), logPath)
        {
            PrefixDeletion = prefix == null ? null : CheckPrefixDeletion(install, prefix, locations, steamRoot)
        };
    }

    // Removes the plan's state patches and old-script files, then Steam's depot folders through SteamDepotDownload. Every
    // item is attempted: a failure is added to Failed and the rest still run.
    public static LinuxUninstallResult Remove(LinuxUninstallPlan plan)
    {
        var removed = new List<string>();
        var failed = new List<string>();
        var left = new List<string>();
        if (plan.Steam is { } steam)
        {
            // Patches first so the app folder can become empty and SteamDepotDownload.Delete can remove it too.
            foreach (var patch in steam.Patches) RemoveItem(patch, removed, failed, left);
            try
            {
                SteamDepotDownload.Delete(steam.Root, steam.Build,
                    (path, error) => failed.Add($"{path}: {error.Message}"), removed.Add);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                failed.Add($"{steam.Root}: {error.Message}");
            }
            foreach (var folder in steam.AppFolders)
                if (Directory.Exists(folder.Path)) left.Add($"{folder.Path} (not removed: it is not empty)");
        }
        foreach (var item in plan.OldScript) RemoveItem(item, removed, failed, left);
        return new(removed, failed, left);
    }

    static string? Prefix(string root, List<string> left)
    {
        try { return WinePath.PrefixFromInstallRoot(root); }
        catch (Exception error) when (error is InvalidOperationException or ArgumentException or NotSupportedException or IOException)
        {
            left.Add($"{root} ({error.Message})");
            return null;
        }
    }

    static IReadOnlyList<LinuxUninstallItem> OldScriptFiles(string? prefix, LinuxUninstallLocations locations, List<string> left)
    {
        var items = new List<LinuxUninstallItem>();
        var configPath = OldConfigPath(locations);
        var config = ReadOldFile(configPath, left);
        var configMatches = config != null && prefix != null && OldConfigBelongs(config.Text, prefix);
        var configReason = configMatches ? null
            : config != null ? "saved settings name another prefix"
            : Exists(configPath) ? "saved settings could not be read"
            : "no saved settings prove they belong to this installation";
        if (configMatches) items.Add(FileItem(config!, "saved settings"));
        else if (config != null) left.Add($"{configPath} (saved settings name another prefix)");
        AddOldCommand(locations, configReason, items, left);
        AddOldMenu(locations, configReason, items, left);
        return items;
    }

    static void AddOldCommand(LinuxUninstallLocations locations, string? configReason,
        List<LinuxUninstallItem> items, List<string> left)
    {
        var commandPath = OldCommandPath(locations);
        var command = ReadOldFile(commandPath, left);
        if (command == null) return;
        if (!OldCommandBelongs(command.Text)) left.Add($"{commandPath} (not the removed installer's command)");
        else if (configReason != null) left.Add($"{commandPath} (left: {configReason})");
        else items.Add(FileItem(command, "command"));
    }

    static void AddOldMenu(LinuxUninstallLocations locations, string? configReason,
        List<LinuxUninstallItem> items, List<string> left)
    {
        var commandPath = OldCommandPath(locations);
        var menuPath = OldMenuEntryPath(locations);
        var menu = ReadOldFile(menuPath, left);
        if (menu == null) return;
        if (!OldMenuEntryBelongs(menu.Text, commandPath)) left.Add($"{menuPath} (not the removed installer's app-menu entry)");
        else if (configReason != null) left.Add($"{menuPath} (left: {configReason})");
        else items.Add(FileItem(menu, "app-menu entry"));
    }

    sealed record SteamScan(SupportedBuild Build, string Root, List<LinuxUninstallItem> Depots,
        List<LinuxUninstallItem> AppFolders, List<LinuxUninstallItem> Patches, HashSet<string> Seen, List<string> Left);

    static LinuxUninstallSteam? SteamFiles(string? steamRoot, SupportedBuild build, List<string> left)
    {
        if (steamRoot == null) return null;
        var scan = new SteamScan(build, steamRoot, [], [], [], new(StringComparer.Ordinal), left);
        if (!TryReadable(steamRoot, left)) return new(build, steamRoot, scan.Depots, scan.AppFolders, scan.Patches);

        var apps = SteamDepotDownload.ContentFolders(steamRoot, build);
        // Only the depot download's own place: the state files in content/app_<app> and directly next to it. Steam's own
        // update/download state elsewhere under steamapps belongs to the installed game and is left alone.
        foreach (var app in apps)
            if (ScanApp(app, scan)) AddPatches(Path.GetDirectoryName(app)!, scan);
        return new(build, steamRoot, scan.Depots, scan.AppFolders, scan.Patches);
    }

    static bool ScanApp(string app, SteamScan scan)
    {
        if (!Exists(app)) return false;
        if (!Within(scan.Root, app) || IsLink(app))
        {
            scan.Left.Add($"{app} (link or outside Steam; not followed)");
            return false;
        }
        var planned = new HashSet<string>(StringComparer.Ordinal);
        foreach (var depot in scan.Build.Depots)
        {
            var folder = Path.Combine(app, "depot_" + depot.Id);
            if (!Directory.Exists(folder)) continue;
            if (!Within(scan.Root, folder) || IsLink(folder))
            {
                scan.Left.Add($"{folder} (link or outside Steam; not followed)");
                continue;
            }
            scan.Depots.Add(new(folder, LinuxUninstallKind.Directory, "depot download", scan.Root));
            planned.Add(LocalInstall.FullPath(folder));
        }
        AddPatches(app, scan);
        planned.UnionWith(scan.Patches.Where(patch => Within(app, patch.Path))
            .Select(patch => LocalInstall.FullPath(patch.Path)));
        try
        {
            if (!Directory.EnumerateFileSystemEntries(app).Any(entry => !planned.Contains(LocalInstall.FullPath(entry))))
                scan.AppFolders.Add(new(app, LinuxUninstallKind.EmptyDirectory, "empty download folder", scan.Root));
            else
                scan.Left.Add($"{app} (other files; only the supported build's download is removed)");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            scan.Left.Add($"{app} (could not read: {error.Message})");
        }
        return true;
    }

    static void AddPatches(string folder, SteamScan scan)
    {
        try
        {
            foreach (var path in Directory.EnumerateFiles(folder, "state_*.patch", SearchOption.TopDirectoryOnly))
                AddPatch(path, scan);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            scan.Left.Add($"{folder} (could not read: {error.Message})");
        }
    }

    static void AddPatch(string path, SteamScan scan)
    {
        if (!StatePatchName(path, scan.Build.AppId)) return;
        if (!Within(scan.Root, path) || IsLink(path))
        {
            scan.Left.Add($"{path} (link or outside Steam; not followed)");
            return;
        }
        if (scan.Seen.Add(LocalInstall.FullPath(path)))
            scan.Patches.Add(new(path, LinuxUninstallKind.File, "download state file", scan.Root));
    }

    static bool TryReadable(string path, List<string> left)
    {
        try { LocalInstall.NoLinks(path); return true; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            left.Add($"{path} ({error.Message})");
            return false;
        }
    }

    public static bool IsLink(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException or IOException or UnauthorizedAccessException) { return false; }
    }

    static LinuxUninstallItem FileItem(OldFile file, string description) =>
        new(file.Path, LinuxUninstallKind.File, description, null, Convert.ToHexString(SHA256.HashData(file.Bytes)));

    // A file whose content was identified and whose bytes are hashed; removal refuses it if it changed since the plan.
    sealed record OldFile(string Path, byte[] Bytes, string Text);

    static OldFile? ReadOldFile(string path, List<string> left)
    {
        if (!Exists(path)) return null;
        try
        {
            LocalInstall.NoLinks(path);
            if (!File.Exists(path)) { left.Add($"{path} (not a regular file)"); return null; }
            var bytes = File.ReadAllBytes(path);
            return new(path, bytes, Encoding.UTF8.GetString(bytes));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            left.Add($"{path} ({error.Message})");
            return null;
        }
    }

    static void RemoveItem(LinuxUninstallItem item, List<string> removed, List<string> failed, List<string> left)
    {
        try
        {
            if (item.Root != null && !Within(item.Root, item.Path))
                throw new InvalidDataException("It is outside " + item.Root + ".");
            if (!Exists(item.Path)) return;
            if (item.Sha256 is { } expected &&
                !Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(item.Path))).Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("It changed since the confirmation.");
            switch (item.Kind)
            {
                case LinuxUninstallKind.File:
                    LocalInstall.NoLinks(item.Path);
                    File.Delete(item.Path);
                    if (Exists(item.Path)) throw new IOException("The file is still there.");
                    removed.Add(item.Path);
                    break;
                case LinuxUninstallKind.Directory:
                    LocalInstall.NoLinks(item.Path);
                    LocalInstall.NoLinksBelow(item.Path);
                    Directory.Delete(item.Path, recursive: true);
                    if (Directory.Exists(item.Path)) throw new IOException("The folder is still there.");
                    removed.Add(item.Path);
                    break;
                case LinuxUninstallKind.EmptyDirectory:
                    LocalInstall.NoLinks(item.Path);
                    if (Directory.EnumerateFileSystemEntries(item.Path).Any()) { left.Add($"{item.Path} (not empty)"); return; }
                    Directory.Delete(item.Path);
                    removed.Add(item.Path);
                    break;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or NotSupportedException)
        {
            failed.Add($"{item.Path}: {error.Message}");
        }
    }

    static string Normalize(string text) => text.Replace("\r\n", "\n", StringComparison.Ordinal);

    // Bash printf %q writes a bare word, '...' or $'...'; paths are ordinary text, so these escapes cover them.
    static string Unquote(string value)
    {
        value = value.Trim();
        if (value.Length >= 2 && value[0] == '\'' && value[^1] == '\'') return value[1..^1];
        if (value.StartsWith("$'", StringComparison.Ordinal) && value.EndsWith('\'')) return Unescape(value[2..^1]);
        if (value.Length >= 2 && value[0] == '"' && value[^1] == '"') return Unescape(value[1..^1]);
        return Unescape(value);
    }

    static string Unescape(string text)
    {
        var result = new StringBuilder(text.Length);
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] != '\\' || i + 1 >= text.Length) { result.Append(text[i]); continue; }
            i++;
            result.Append(text[i] switch
            {
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                '\\' => '\\',
                '\'' => '\'',
                '"' => '"',
                ' ' => ' ',
                _ => '\\'
            });
            if (text[i] is not ('n' or 'r' or 't' or '\\' or '\'' or '"' or ' ')) result.Append(text[i]);
        }
        return result.ToString();
    }
}
