using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using HellForge.Core;

namespace HellForge.Launcher;

// Settings > Installation > Uninstall on Linux: the exact outside paths the confirmation lists, their removal before the
// launcher exits, and the helper that deletes the installation (and, when the player asks, the prefix HellForge made) once
// it has. Steam's depot download and state files are a separate choice, off by default.
public partial class MainWindow
{
    // Removes what the old shell installer left outside the installation first, and Steam's download when its box is ticked,
    // then starts a small script that deletes the installation, its shortcut and the owned prefix once this launcher has
    // exited. A failed path is reported in the log and never stops the others. AppArmor needs sudo, so the launcher only
    // shows the commands for it.
    async Task UninstallLinux(InstallLayout install, bool deletePrefix, bool deleteSteam)
    {
        try { UninstallSafety(install.Root); }
        catch (InvalidDataException refused) { UninstallText.Text = Privacy.Scrub(refused.Message); return; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            UninstallText.Text = Privacy.Scrub(error.Message);
            return;
        }

        try
        {
            var plan = (linuxUninstallPlan ?? BuildLinuxUninstallPlan(install)) with { RemoveSteam = deleteSteam };
            string? prefix = null;
            if (deletePrefix)
            {
                if (plan.PrefixDeletion is not { Offered: true } offered)
                {
                    UninstallText.Text = "The Proton prefix was not offered for deletion; nothing was removed.";
                    UninstallButton.IsEnabled = true;
                    return;
                }
                // The player may have started the game, changed the marker, or made a link since the confirmation appeared.
                // Check the path as it was chosen so a WINEPREFIX that uses a link above the prefix still matches.
                var fresh = await Task.Run(() => LinuxUninstall.CheckPrefixDeletion(install, plan.Prefix ?? offered.Prefix,
                    UninstallLocations(), SteamLocator.SteamRoot(install)));
                if (!fresh.Offered)
                {
                    UninstallText.Text = Privacy.Scrub("The Proton prefix was not deleted: " + fresh.Reason + " Nothing was removed.");
                    UninstallButton.IsEnabled = true;
                    return;
                }
                prefix = fresh.Prefix;
            }
            UninstallButton.IsEnabled = false;
            UninstallText.Text = deleteSteam
                ? "Removing Steam's download and the removed installer's files..."
                : "Removing the removed installer's files (Steam's download is left)...";
            LinuxUninstallResult result;
            try { result = await Task.Run(() => LinuxUninstall.Remove(plan)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or InvalidOperationException)
            {
                result = new([], [error.Message], []);
            }
            WriteLinuxUninstallLog(plan, result);
            var removing = prefix == null ? "this installation" : "this installation and its Proton prefix";
            UninstallText.Text = result.Failed.Count == 0
                ? $"Removing {removing} after the launcher closes..."
                : $"{result.Failed.Count} item(s) could not be removed; the details are in {plan.LogPath}. Removing {removing} after the launcher closes...";
            StartLinuxUninstallHelper(install, plan, prefix);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
        {
            UninstallText.Text = Privacy.Scrub(error.Message);
            UninstallButton.IsEnabled = true;
        }
    }

    static LinuxUninstallLocations UninstallLocations() => new(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        Environment.GetEnvironmentVariable("XDG_CONFIG_HOME"), Environment.GetEnvironmentVariable("XDG_DATA_HOME"));

    LinuxUninstallPlan BuildLinuxUninstallPlan(InstallLayout install)
    {
        var locations = UninstallLocations();
        var log = Path.Combine(Path.GetTempPath(), $"hellforge-uninstall-{Environment.ProcessId}-{Guid.NewGuid():N}.log");
        var plan = LinuxUninstall.Plan(install, SupportedBuild.Current, locations, SteamLocator.SteamRoot(install), log);
        var launcher = LocalInstall.FullPath(Path.Combine(install.App, "launcher-linux", "HellForge.Launcher"));
        var shortcut = LinuxDesktopShortcutPath();
        var expected = LinuxDesktopExecLine(launcher);
        try
        {
            if (LinuxUninstall.Exists(shortcut))
            {
                if (!LinuxUninstall.IsLink(shortcut) &&
                    File.ReadAllText(shortcut).Split('\n').Any(line => line == expected))
                    return plan with { Shortcut = shortcut, ShortcutExec = expected, ShortcutRemovable = true };
                return plan with { Shortcut = shortcut, ShortcutExec = expected,
                    Left = [.. plan.Left, $"{shortcut} (changed; left)"] };
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return plan with { Shortcut = shortcut, ShortcutExec = expected,
                Left = [.. plan.Left, $"{shortcut} (could not read: {error.Message}; left)"] };
        }
        return plan with { ShortcutExec = expected };
    }

    // Fills the warning and the separate Steam choice: off by default, with the exact paths it would delete listed under
    // it. The plan scans Steam even when the box is off, so the paths are known before the player chooses.
    void FillLinuxUninstallWarning(InstallLayout install, LinuxUninstallPlan plan)
    {
        UninstallWarning.Text = LinuxUninstallWarning(install, plan);
        UninstallSteamBox.IsChecked = false;
        UninstallSteamBox.IsVisible = false;
        UninstallSteamPaths.Text = "";
        UninstallSteamPaths.IsVisible = false;
        if (plan.Steam is not { } steam ||
            (steam.Depots.Count == 0 && steam.AppFolders.Count == 0 && steam.Patches.Count == 0))
        {
            UninstallSteamBox.Content = "";
            return;
        }
        UninstallSteamBox.Content = $"Also delete Steam's depot download and its state files for build {steam.Build.BuildId}: this deletes the depot folders and state patches in this Steam library, including a download made for another HellForge installation";
        UninstallSteamPaths.Text = LinuxUninstallSteamPaths(steam);
        UninstallSteamBox.IsVisible = true;
        UninstallSteamPaths.IsVisible = true;
    }

    static string LinuxUninstallWarning(InstallLayout install, LinuxUninstallPlan plan)
    {
        var text = new StringBuilder();
        text.AppendLine("Uninstall removes:");
        text.AppendLine($"  - this installation: {install.Root} (game copy, profiles, progress, mods, saves and logs)");
        if (plan.ShortcutRemovable) text.AppendLine($"  - its desktop shortcut after the launcher closes: {plan.Shortcut}");
        else if (plan.Shortcut != null) text.AppendLine($"  - no desktop shortcut (the one at {plan.Shortcut} is not unchanged HellForge's; it is left)");
        else text.AppendLine("  - a desktop shortcut: none found");
        AppendSteamRemovals(text, plan);
        AppendOldScriptRemovals(text, plan.OldScript);
        AppendPrefixRemoval(text, plan);
        text.AppendLine();
        text.AppendLine("It leaves:");
        var offeredPrefix = plan.PrefixDeletion is { Offered: true } offer ? offer.Prefix : null;
        text.AppendLine(plan.Prefix is null
            ? "  - the Proton prefix (its path could not be read)"
            : offeredPrefix != null
                ? $"  - the Proton prefix: {offeredPrefix} (kept unless you tick the prefix box)"
                : $"  - the Proton prefix: {plan.Prefix} (left: {plan.PrefixDeletion?.Reason ?? "it could not be checked"})");
        text.AppendLine("  - Steam, HELLDIVERS 2, your Proton versions and every other file outside this installation");
        AppendSteamLeft(text, plan);
        text.AppendLine("  - Steam's own update and download state elsewhere under steamapps");
        if (plan.AppArmorPaths.Count > 0)
        {
            text.AppendLine("  - the AppArmor namespace permission (the launcher cannot run sudo):");
            foreach (var path in plan.AppArmorPaths) text.AppendLine("      " + path);
            text.AppendLine("    remove it with these commands (Copy sudo commands copies them):");
            foreach (var command in plan.AppArmorCommands) text.AppendLine("      " + command);
        }
        else text.AppendLine("  - the AppArmor namespace permission: none found");
        foreach (var item in plan.Left) text.AppendLine("  - " + item);
        text.AppendLine();
        text.AppendLine($"Anything that cannot be removed is listed in {plan.LogPath}. HellForge closes when it is done.");
        return text.ToString();
    }

    // The Steam box is a separate choice, off by default, so the removal list only claims its paths when the plan carries
    // the choice. The scan still ran when it was off, so the confirmation can name what ticking it would delete.
    static void AppendSteamRemovals(StringBuilder text, LinuxUninstallPlan plan)
    {
        if (plan.Steam is not { } steam)
        {
            text.AppendLine("  - Steam's download of the supported build: Steam's folder was not found, so none was removed");
            return;
        }
        if (!plan.RemoveSteam)
        {
            var count = steam.Depots.Count + steam.AppFolders.Count + steam.Patches.Count;
            text.AppendLine(count == 0
                ? $"  - Steam's depot download and state files for build {steam.Build.BuildId}: none found"
                : $"  - Steam's depot download and state files for build {steam.Build.BuildId}: only if you tick the Steam box below");
            return;
        }
        if (steam.Depots.Count == 0 && steam.AppFolders.Count == 0)
            text.AppendLine($"  - Steam's depot download for build {steam.Build.BuildId}: none found");
        foreach (var item in steam.Depots) text.AppendLine($"  - Steam's depot download: {item.Path}");
        foreach (var item in steam.AppFolders) text.AppendLine($"  - Steam's download folder once empty: {item.Path}");
        if (steam.Patches.Count == 0)
            text.AppendLine($"  - Steam's download state files in or next to its content/app_{steam.Build.AppId} folder: none found");
        foreach (var item in steam.Patches)
            text.AppendLine($"  - Steam's download state file in or next to its content/app_{steam.Build.AppId} folder: {item.Path}");
    }

    // The Steam box is unticked in the confirmation the plan is built for: name the exact paths it leaves unless the player
    // ticks it. The paths come from the same scan the box uses, so they match what ticking it would delete.
    static void AppendSteamLeft(StringBuilder text, LinuxUninstallPlan plan)
    {
        if (plan.RemoveSteam || plan.Steam is not { } steam) return;
        foreach (var item in steam.Depots)
            text.AppendLine($"  - Steam's depot download (kept unless you tick the Steam box): {item.Path}");
        foreach (var item in steam.AppFolders)
            text.AppendLine($"  - Steam's download folder once empty (kept unless you tick the Steam box): {item.Path}");
        foreach (var item in steam.Patches)
            text.AppendLine($"  - Steam's download state file (kept unless you tick the Steam box): {item.Path}");
    }

    // The Steam box lists its exact paths under itself before it is ticked, so the player can see what it would delete.
    static string LinuxUninstallSteamPaths(LinuxUninstallSteam steam)
    {
        var lines = new List<string>();
        foreach (var item in steam.Depots) lines.Add("Steam's depot download: " + item.Path);
        foreach (var item in steam.AppFolders) lines.Add("Steam's download folder once empty: " + item.Path);
        foreach (var item in steam.Patches) lines.Add("Steam's download state file: " + item.Path);
        return string.Join(Environment.NewLine, lines);
    }

    static void AppendOldScriptRemovals(StringBuilder text, IReadOnlyList<LinuxUninstallItem> oldScript)
    {
        if (oldScript.Count == 0)
        {
            text.AppendLine("  - the removed shell installer's files: none found");
            return;
        }
        foreach (var item in oldScript) text.AppendLine($"  - the removed shell installer's {item.Description}: {item.Path}");
    }

    // The prefix checkbox is a separate choice, off by default. A prefix without this installer's marker (HELLDIVERS 2's own
    // compatdata prefix, one the player chose, or one from an earlier HellForge version) is never offered.
    static void AppendPrefixRemoval(StringBuilder text, LinuxUninstallPlan plan)
    {
        if (plan.PrefixDeletion is { Offered: true } offer)
            text.AppendLine($"  - the Proton prefix only if you tick the box below: {offer.Prefix} (the game copy, every profile, every mod and everything else in it)");
        else
            text.AppendLine("  - the Proton prefix: not offered; only a prefix this installer made is. Prefixes from earlier HellForge versions have no marker, so delete one by hand if you want it gone");
    }

    static void WriteLinuxUninstallLog(LinuxUninstallPlan plan, LinuxUninstallResult result)
    {
        try
        {
            var text = new StringBuilder();
            text.AppendLine("HellForge uninstall");
            text.AppendLine("Installation: " + plan.Root);
            text.AppendLine("Started: " + DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture));
            foreach (var path in result.Removed) text.AppendLine("removed: " + path);
            foreach (var problem in result.Failed) text.AppendLine("failed: " + problem);
            foreach (var item in result.Left) text.AppendLine("left: " + item);
            foreach (var command in plan.AppArmorCommands) text.AppendLine("left for sudo: " + command);
            File.WriteAllText(plan.LogPath, text.ToString());
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    void StartLinuxUninstallHelper(InstallLayout install, LinuxUninstallPlan plan, string? prefix)
    {
        var script = Path.Combine(Path.GetTempPath(), $"hellforge-uninstall-{Guid.NewGuid():N}.sh");
        var helperText = """
            #!/usr/bin/env bash
            set -u
            root=$1
            launcher_pid=$2
            shortcut=${3:-}
            expected_exec=${4:-}
            log=$5
            prefix=${6:-}
            say() { printf '%s\n' "$*" >>"$log" 2>/dev/null || true; }
            # A link above the prefix is followed to the real folder, so the deletion names the same place the offer does;
            # the prefix's own last component is checked separately and may not be a link. Links inside the resolved
            # prefix are removed as links: rm -rf --one-file-system never follows them.
            resolve_prefix() {
              local path=$1 parent name real
              case "$path" in
                /*/*) parent=${path%/*}; name=${path##*/} ;;
                /*) parent=/; name=${path#/} ;;
                *) return 1 ;;
              esac
              [ -n "$name" ] || return 1
              real=$(cd -- "$parent" 2>/dev/null && pwd -P) || return 1
              printf '%s\n' "${real%/}/$name"
            }
            @PREFIX_IN_USE@
            failed=0
            while kill -0 "$launcher_pid" 2>/dev/null; do sleep 1; done
            if [ -n "$shortcut" ] && { [ -e "$shortcut" ] || [ -L "$shortcut" ]; }; then
              if [ -L "$shortcut" ]; then
                say "failed: $shortcut (link; refused)"; failed=1
              elif [ ! -f "$shortcut" ]; then
                say "failed: $shortcut (not a file)"; failed=1
              elif grep -Fqx -- "$expected_exec" "$shortcut"; then
                if rm -f -- "$shortcut"; then say "removed: $shortcut"; else say "failed: $shortcut"; failed=1; fi
              else
                say "left: $shortcut (changed)"
              fi
            elif [ -n "$shortcut" ]; then
              say "not found: $shortcut"
            fi
            if [ -L "$root" ]; then
              say "failed: $root (link; refused)"; failed=1
            elif [ -e "$root" ]; then
              rm -rf --one-file-system -- "$root" 2>/dev/null || true
              if [ -e "$root" ] || [ -L "$root" ]; then say "failed: $root"; failed=1; else say "removed: $root"; fi
            else
              say "not found: $root"
            fi
            if [ -n "$prefix" ]; then
              if [ -L "$prefix" ]; then
                say "failed: $prefix (link; refused)"; failed=1
              elif [ ! -e "$prefix" ]; then
                say "not found: $prefix"
              elif [ ! -d "$prefix" ]; then
                say "failed: $prefix (not a folder)"; failed=1
              elif ! resolved=$(resolve_prefix "$prefix"); then
                say "failed: $prefix (its folder could not be resolved)"; failed=1
              else
                prefix=$resolved
                deadline=$((SECONDS + 30))
                while prefix_in_use; do
                  if [ "$SECONDS" -ge "$deadline" ]; then
                    say "failed: $prefix (still in use by $pfx_in_use_names)"; failed=1; prefix=""
                    break
                  fi
                  sleep 1
                done
                if [ -n "$prefix" ]; then
                  # --one-file-system keeps the deletion on the prefix's filesystem; links inside the prefix are removed
                  # as links and never followed.
                  rm -rf --one-file-system -- "$prefix" 2>/dev/null || true
                  if [ -e "$prefix" ] || [ -L "$prefix" ]; then say "failed: $prefix"; failed=1; else say "removed: $prefix"; fi
                fi
              fi
            fi
            if [ "$failed" -eq 0 ]; then
              say "Uninstall finished: every planned path was removed."
            else
              say "Uninstall finished with items that could not be removed; see above."
            fi
            rm -f -- "$0" 2>/dev/null || true
            """;
        File.WriteAllText(script, LinuxHelperScripts.IncludePrefixInUse(helperText));
        var start = new ProcessStartInfo("bash") { WorkingDirectory = Path.GetTempPath(), UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[] { script, install.Root, Environment.ProcessId.ToString(CultureInfo.InvariantCulture),
                     plan.ShortcutRemovable ? plan.Shortcut ?? "" : "", plan.ShortcutExec ?? "", plan.LogPath, prefix ?? "" })
            start.ArgumentList.Add(argument);
        using var cleanup = Process.Start(start) ?? throw new IOException("The Linux uninstall helper did not start.");
        Close();
    }
}
