using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using HellForge.Core;

namespace HellForge.Launcher;

// Settings > Installation > Uninstall on Linux: the exact outside paths the confirmation lists, their removal before the
// launcher exits, and the helper that deletes the installation once it has.
public partial class MainWindow
{
    // Removes what the old shell installer and Steam left outside the installation first, then starts a small script that
    // deletes the installation and its shortcut once this launcher has exited. A failed path is reported in the log and
    // never stops the others. AppArmor needs sudo, so the launcher only shows the commands for it.
    async Task UninstallLinux(InstallLayout install)
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
            var plan = linuxUninstallPlan ?? BuildLinuxUninstallPlan(install);
            UninstallButton.IsEnabled = false;
            UninstallText.Text = "Removing Steam's download and the removed installer's files...";
            LinuxUninstallResult result;
            try { result = await Task.Run(() => LinuxUninstall.Remove(plan)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or InvalidOperationException)
            {
                result = new([], [error.Message], []);
            }
            WriteLinuxUninstallLog(plan, result);
            UninstallText.Text = result.Failed.Count == 0
                ? "Removing this installation after the launcher closes..."
                : $"{result.Failed.Count} item(s) could not be removed; the details are in {plan.LogPath}. Removing this installation after the launcher closes...";
            StartLinuxUninstallHelper(install, plan);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
        {
            UninstallText.Text = Privacy.Scrub(error.Message);
            UninstallButton.IsEnabled = true;
        }
    }

    LinuxUninstallPlan BuildLinuxUninstallPlan(InstallLayout install)
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var locations = new LinuxUninstallLocations(home,
            Environment.GetEnvironmentVariable("XDG_CONFIG_HOME"), Environment.GetEnvironmentVariable("XDG_DATA_HOME"));
        var log = Path.Combine(Path.GetTempPath(), $"hellforge-uninstall-{Environment.ProcessId}-{Guid.NewGuid():N}.log");
        var plan = LinuxUninstall.Plan(install, SupportedBuild.Current, locations, SteamLocator.SteamRoot(), log);
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

    static string LinuxUninstallWarning(InstallLayout install, LinuxUninstallPlan plan)
    {
        var text = new StringBuilder();
        text.AppendLine("Uninstall removes:");
        text.AppendLine($"  - this installation: {install.Root} (game copy, profiles, progress, mods, saves and logs)");
        if (plan.ShortcutRemovable) text.AppendLine($"  - its desktop shortcut after the launcher closes: {plan.Shortcut}");
        else if (plan.Shortcut != null) text.AppendLine($"  - no desktop shortcut (the one at {plan.Shortcut} is not unchanged HellForge's; it is left)");
        else text.AppendLine("  - a desktop shortcut: none found");
        AppendSteamRemovals(text, plan.Steam);
        AppendOldScriptRemovals(text, plan.OldScript);
        text.AppendLine();
        text.AppendLine("It leaves:");
        text.AppendLine(plan.Prefix is null
            ? "  - the Proton prefix (its path could not be read)"
            : $"  - the Proton prefix: {plan.Prefix} (and everything in it)");
        text.AppendLine("  - Steam, HELLDIVERS 2, your Proton versions and every other file outside this installation");
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

    static void AppendSteamRemovals(StringBuilder text, LinuxUninstallSteam? steam)
    {
        if (steam == null)
        {
            text.AppendLine("  - Steam's download of the supported build: Steam's folder was not found, so none was removed");
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

    static void AppendOldScriptRemovals(StringBuilder text, IReadOnlyList<LinuxUninstallItem> oldScript)
    {
        if (oldScript.Count == 0)
        {
            text.AppendLine("  - the removed shell installer's files: none found");
            return;
        }
        foreach (var item in oldScript) text.AppendLine($"  - the removed shell installer's {item.Description}: {item.Path}");
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

    void StartLinuxUninstallHelper(InstallLayout install, LinuxUninstallPlan plan)
    {
        var script = Path.Combine(Path.GetTempPath(), $"hellforge-uninstall-{Guid.NewGuid():N}.sh");
        File.WriteAllText(script, """
            #!/usr/bin/env bash
            set -u
            root=$1
            launcher_pid=$2
            shortcut=${3:-}
            expected_exec=${4:-}
            log=$5
            say() { printf '%s\n' "$*" >>"$log" 2>/dev/null || true; }
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
            if [ "$failed" -eq 0 ]; then
              say "Uninstall finished: every planned path was removed."
            else
              say "Uninstall finished with items that could not be removed; see above."
            fi
            rm -f -- "$0" 2>/dev/null || true
            """);
        var start = new ProcessStartInfo("bash") { WorkingDirectory = Path.GetTempPath(), UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[] { script, install.Root, Environment.ProcessId.ToString(CultureInfo.InvariantCulture),
                     plan.ShortcutRemovable ? plan.Shortcut ?? "" : "", plan.ShortcutExec ?? "", plan.LogPath })
            start.ArgumentList.Add(argument);
        using var cleanup = Process.Start(start) ?? throw new IOException("The Linux uninstall helper did not start.");
        Close();
    }
}
