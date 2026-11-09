using System.Diagnostics;
using HellForge.Core;

// Area I: one Steam and Proton search in SteamLocator and linux/hf-steam.sh, a Proton the player picks outside the list,
// a Steam folder outside the usual places, and the saved choices the wrapper reads for an external launch.
static class SteamPathTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux Proton discovery searches every library, the extra tool paths and the system tool folders", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Proton discovery)"); return; }
            var sandbox = Path.Combine(root, "steam-paths-proton");
            var steam = Path.Combine(sandbox, "Steam");
            var second = Path.Combine(sandbox, "Library2");
            var old = Proton(Path.Combine(steam, "steamapps", "common", "Proton 9.0", "proton"));
            var newest = Proton(Path.Combine(steam, "steamapps", "common", "Proton 10.0", "proton"));
            var experimental = Proton(Path.Combine(steam, "steamapps", "common", "Proton - Experimental", "proton"));
            var secondary = Proton(Path.Combine(second, "steamapps", "common", "Proton 8.0", "proton"));
            var steamTool = Proton(Path.Combine(steam, "compatibilitytools.d", "GE-Proton10-25", "proton"), metadata: true);
            var ignored = Proton(Path.Combine(steam, "compatibilitytools.d", "No metadata", "proton"));
            var extra = Path.Combine(sandbox, "extra-tools");
            var extraTool = Proton(Path.Combine(extra, "Proton 11.0", "proton"), metadata: true);
            var system = Path.Combine(sandbox, "system-tools");
            var systemTool = Proton(Path.Combine(system, "Proton 12.0", "proton"), metadata: true);
            var configured = Proton(Path.Combine(sandbox, "configured", "Proton Custom", "proton"));
            File.WriteAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
                "\"libraryfolders\" { \"0\" { \"path\" \"" + steam.Replace("\\", "\\\\") + "\" } \"1\" { \"path\" \"" +
                second.Replace("\\", "\\\\") + "\" } }");

            var versions = SteamLocator.ProtonVersionsFrom(steam, [extra], [system], configured);
            var expected = new[] { configured, secondary, old, newest, steamTool, extraTool, systemTool, experimental };
            check(versions.Select(version => version.ScriptPath).SequenceEqual(expected),
                "The Proton search or its sorting order is wrong: " + string.Join(", ", versions.Select(version => version.Name)));
            check(versions.All(version => version.ScriptPath != ignored),
                "A compatibilitytools.d folder without compatibilitytool.vdf was listed.");
        });

        test("Linux Proton discovery accepts a Proton outside the list only as an executable proton with files or dist", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Proton choice)"); return; }
            var sandbox = Path.Combine(root, "steam-paths-other");
            var good = Proton(Path.Combine(sandbox, "GE-Proton10-25", "proton"));
            var existing = new ProtonVersion("Proton 9.0", Proton(Path.Combine(sandbox, "Proton 9.0", "proton")));
            check(SteamLocator.ProtonProblem(good) == null, "A normal Proton folder was refused: " + SteamLocator.ProtonProblem(good));
            var list = SteamLocator.IncludingSelected([existing], good);
            check(list.Count == 2 && list[1].ScriptPath == good, "The picked Proton was not added to the list once.");
            check(SteamLocator.IncludingSelected(list, good).Count == 2, "The picked Proton was added twice.");
            var wrongName = Script(Path.Combine(sandbox, "wrong", "wine"));
            check(SteamLocator.ProtonProblem(wrongName)?.Contains("named proton", StringComparison.Ordinal) == true,
                "A file not named proton was accepted: " + SteamLocator.ProtonProblem(wrongName));
            var notExecutable = Proton(Path.Combine(sandbox, "not-executable", "proton"), executable: false);
            check(SteamLocator.ProtonProblem(notExecutable)?.Contains("not executable", StringComparison.Ordinal) == true,
                "A proton file that is not executable was accepted: " + SteamLocator.ProtonProblem(notExecutable));
            var noRuntime = Proton(Path.Combine(sandbox, "no-runtime", "proton"), runtime: false);
            check(SteamLocator.ProtonProblem(noRuntime)?.Contains("no files or dist", StringComparison.Ordinal) == true,
                "A Proton folder without files or dist was accepted: " + SteamLocator.ProtonProblem(noRuntime));
            check(SteamLocator.ProtonProblem(Path.Combine(sandbox, "missing", "proton"))?.Contains("does not exist", StringComparison.Ordinal) == true,
                "A missing proton script was accepted.");
            check(SteamLocator.ProtonProblem(Directory.CreateDirectory(Path.Combine(sandbox, "a-folder")).FullName)
                ?.Contains("folder", StringComparison.Ordinal) == true, "A folder was accepted as a proton script.");
            var install = new InstallLayout(Path.Combine(sandbox, "install"));
            Directory.CreateDirectory(install.Root);
            SteamLocator.SaveProtonSelection(install, good);
            check(SteamLocator.SelectedProtonScript(install) == good, "The picked Proton was not saved in proton-selection.json.");
        });

        test("Linux finds Steam through the .steam links and saves a chosen folder in the installation", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Steam choice)"); return; }
            var sandbox = Path.Combine(root, "steam-paths-root");
            var realRoot = SteamRoot(Path.Combine(sandbox, "var", "home", "Steam"));
            var home = Directory.CreateDirectory(Path.Combine(sandbox, "home")).FullName;
            Directory.CreateDirectory(Path.Combine(home, ".steam"));
            Directory.CreateSymbolicLink(Path.Combine(home, ".steam", "root"), realRoot);
            check(SteamLocator.FindSteamRoot(home, null) == realRoot, "~/.steam/root did not find the Steam folder.");

            var secondHome = Directory.CreateDirectory(Path.Combine(sandbox, "other-home")).FullName;
            Directory.CreateDirectory(Path.Combine(secondHome, ".steam"));
            var secondRoot = SteamRoot(Path.Combine(sandbox, "var", "second", "Steam"));
            Directory.CreateSymbolicLink(Path.Combine(secondHome, ".steam", "steam"), secondRoot);
            check(SteamLocator.FindSteamRoot(secondHome, null) == secondRoot, "~/.steam/steam did not find the Steam folder.");
            check(SteamLocator.FindSteamRoot(home, secondRoot) == secondRoot, "A configured Steam folder did not win.");
            check(SteamLocator.FindSteamRoot(Path.Combine(sandbox, "empty-home"), null) == null, "A missing Steam folder was reported.");

            check(SteamLocator.SteamFolderProblem(realRoot) == null, "A folder with steamapps was refused.");
            check(SteamLocator.SteamFolderProblem(Directory.CreateDirectory(Path.Combine(sandbox, "no-steamapps")).FullName)
                ?.Contains("no steamapps", StringComparison.Ordinal) == true, "A folder without steamapps was accepted.");
            check(SteamLocator.SteamFolderProblem(Path.Combine(sandbox, "missing"))
                ?.Contains("does not exist", StringComparison.Ordinal) == true, "A missing Steam folder was accepted.");

            var install = new InstallLayout(Path.Combine(sandbox, "install"));
            Directory.CreateDirectory(install.Root);
            var configured = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH");
            try
            {
                Environment.SetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH", null);
                SteamLocator.SaveSteamSelection(install, realRoot);
                check(SteamLocator.SelectedSteamRoot(install) == realRoot, "The chosen Steam folder was not saved.");
                check(SteamLocator.SteamRoot(install) == realRoot, "The saved Steam folder was not used by SteamLocator.");
            }
            finally { Environment.SetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH", configured); }
            File.WriteAllText(install.SteamSelection,
                "{\"steamRoot\":\"" + Path.Combine(sandbox, "missing").Replace("\\", "\\\\") + "\"}\n");
            check(SteamLocator.SelectedSteamRoot(install) == null, "A saved Steam folder that no longer exists was used.");
        });

        test("The wrapper picks the same Proton default as SteamLocator and reads the saved choices", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux wrapper lookup)"); return; }
            var sandbox = Path.Combine(root, "steam-paths-wrapper");
            var steam = Path.Combine(sandbox, "Steam");
            var second = Path.Combine(sandbox, "Library2");
            Proton(Path.Combine(steam, "steamapps", "common", "Proton 9.0", "proton"));
            Proton(Path.Combine(steam, "steamapps", "common", "Proton 10.0", "proton"));
            Proton(Path.Combine(steam, "steamapps", "common", "Proton - Experimental", "proton"));
            Proton(Path.Combine(second, "steamapps", "common", "Proton 8.0", "proton"));
            Proton(Path.Combine(steam, "compatibilitytools.d", "GE-Proton10-25", "proton"), metadata: true);
            File.WriteAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
                "\"libraryfolders\" { \"0\" { \"path\" \"" + steam.Replace("\\", "\\\\") + "\" } \"1\" { \"path\" \"" +
                second.Replace("\\", "\\\\") + "\" } }");
            var expected = SteamLocator.ProtonVersionsFrom(steam, [], [], null).Select(version => version.ScriptPath).ToArray();
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-steam.sh");
            check(File.Exists(helper), "linux/hf-steam.sh was not found: " + helper);

            var listed = RunBash("source \"$1\"; hf_proton_scripts \"$2\" \"\" \"\"", helper, steam);
            check(listed.ExitCode == 0, "The wrapper's Proton search failed: " + listed.Error);
            check(listed.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .SequenceEqual(expected), "The wrapper listed a different Proton search:\n" + listed.Output);

            var defaultProton = RunBash("source \"$1\"; hf_default_proton \"$2\" \"\" \"\"", helper, steam);
            check(defaultProton.ExitCode == 0 && defaultProton.Output.Trim() == expected.Last(),
                "The wrapper's default Proton differs from SteamLocator's: " + defaultProton.Output.Trim());

            var install = Directory.CreateDirectory(Path.Combine(sandbox, "install")).FullName;
            File.WriteAllText(Path.Combine(install, "steam-selection.json"), "{\"steamRoot\":\"" + steam.Replace("\\", "\\\\") + "\"}\n");
            File.WriteAllText(Path.Combine(install, "proton-selection.json"), "{\"scriptPath\":\"" + expected.Last() + "\"}\n");
            var saved = RunBash("source \"$1\"; hf_saved_steam_root \"$2\"; hf_saved_proton \"$2\"", helper, install);
            check(saved.ExitCode == 0 && saved.Output.Trim().Replace("\r", "").Split('\n')
                .SequenceEqual([LocalInstall.ResolveLinks(steam), expected.Last()]), "The wrapper did not read the saved Steam and Proton choices: " + saved.Output);
        });
    }

    static (int ExitCode, string Output, string Error) RunBash(string command, params string[] arguments)
    {
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add(command);
        start.ArgumentList.Add("bash");
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new InvalidDataException("bash did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        return (process.ExitCode, output.GetAwaiter().GetResult(), error.GetAwaiter().GetResult());
    }

    static string Proton(string script, bool metadata = false, bool executable = true, bool runtime = true)
    {
        Script(script, executable);
        var folder = Path.GetDirectoryName(script)!;
        if (metadata)
            File.WriteAllText(Path.Combine(folder, "compatibilitytool.vdf"),
                "\"compatibilitytools\" { \"compat_tools\" { \"test\" { \"display_name\" \"Test\" \"install_path\" \".\" } } }");
        if (runtime) Directory.CreateDirectory(Path.Combine(folder, "files"));
        return LocalInstall.FullPath(script);
    }

    static string Script(string path, bool executable = true)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "#!/bin/sh\n");
        // The tests return early on other platforms; the guard also keeps the platform analyzer happy.
        if (OperatingSystem.IsLinux())
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite |
                (executable ? UnixFileMode.UserExecute : UnixFileMode.None));
        return LocalInstall.FullPath(path);
    }

    static string SteamRoot(string path)
    {
        Directory.CreateDirectory(Path.Combine(path, "steamapps"));
        return LocalInstall.FullPath(path);
    }

    static string RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        return Environment.CurrentDirectory;
    }
}
