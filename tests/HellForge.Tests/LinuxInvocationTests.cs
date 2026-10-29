using System.Diagnostics;
using HellForge.Core;

// J.3 phase 2: every C# Proton invocation goes through LinuxProtonInvocation, which returns the argv and the environment
// the wrapper would use. The fixtures are synthetic Proton/runtime trees; no Steam, Proton or container is started.
static class LinuxInvocationTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        _ = reject;
        test("Linux Proton invocations keep the host verbs and use the selected manifest commandline", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Proton invocation)"); return; }
            var steam = Path.Combine(root, "invocation-host");
            var protonFolder = Path.Combine(steam, "steamapps", "common", "Proton - Experimental");
            var proton = MakeProton(protonFolder, "/proton %verb%", null);
            var prefix = Path.Combine(root, "invocation-host-prefix");

            var run = LinuxProtonInvocation.Create(proton, steam, prefix, "run", ["reg", "query", "HKCU\\Software"]);
            check(run.Arguments.SequenceEqual([proton, "run", "reg", "query", "HKCU\\Software"]),
                "A host run commandline changed: " + string.Join(" | ", run.Arguments));
            check(!run.Environment.ContainsKey("STEAM_COMPAT_TOOL_PATHS") &&
                run.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] == LocalInstall.ResolveLinks(steam) &&
                run.Environment["STEAM_COMPAT_DATA_PATH"] == LocalInstall.ResolveLinks(prefix) &&
                run.Environment["STEAM_COMPAT_APP_ID"] == LocalInstall.AppId,
                "A host invocation acquired container-only environment.");
            // Measured: SteamAppId together with SteamGameId makes Steam count a helper call as a HELLDIVERS 2 session.
            var previous = Environment.GetEnvironmentVariable("SteamAppId");
            Environment.SetEnvironmentVariable("SteamAppId", "553850");
            try
            {
                check(!run.Environment.ContainsKey("SteamAppId") && !run.StartInfo(root).Environment.ContainsKey("SteamAppId"),
                    "A helper Proton call carries SteamAppId, so Steam would count it as a game session.");
            }
            finally { Environment.SetEnvironmentVariable("SteamAppId", previous); }

            var wineboot = LinuxProtonInvocation.Create(proton, steam, prefix, "run", ["wineboot", "-u"]);
            check(wineboot.Arguments.SequenceEqual([proton, "run", "wineboot", "-u"]),
                "wineboot did not keep run: " + string.Join(" | ", wineboot.Arguments));
            var start = LinuxProtonInvocation.Create(proton, steam, prefix, "waitforexitandrun",
                ["C:\\windows\\system32\\start.exe", "/wait", "/b", "C:\\tool.exe"]);
            check(start.Arguments.SequenceEqual([proton, "waitforexitandrun", "C:\\windows\\system32\\start.exe", "/wait", "/b", "C:\\tool.exe"]),
                "start.exe did not keep waitforexitandrun: " + string.Join(" | ", start.Arguments));

            MakeManifest(protonFolder, "/proton --host-token %verb%", null);
            var token = LinuxProtonInvocation.Create(proton, steam, prefix, "run", ["x"]);
            check(token.Arguments.SequenceEqual([proton, "--host-token", "run", "x"]),
                "The extra host manifest token was lost: " + string.Join(" | ", token.Arguments));

            File.Delete(Path.Combine(protonFolder, "toolmanifest.vdf"));
            var direct = LinuxProtonInvocation.Create(proton, steam, prefix, "run", ["x"]);
            check(direct.Arguments.SequenceEqual([proton, "run", "x"]),
                "A missing manifest did not fall back to proton <verb>: " + string.Join(" | ", direct.Arguments));
        });

        test("Linux Proton invocations use the measured container chain and its environment", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux container invocation)"); return; }
            var tree = ContainerTree(Path.Combine(root, "invocation-container"));
            var game = Path.Combine(root, "invocation-game");
            Directory.CreateDirectory(game);
            var prefix = Path.Combine(root, "invocation-prefix");
            var shader = Path.Combine(root, "invocation-shader");
            var invocation = LinuxProtonInvocation.Create(tree.Proton, tree.Steam, prefix, "waitforexitandrun",
                ["C:\\windows\\system32\\start.exe", "/wait"], new(InstallPath: game, Mounts: [game], ShaderPath: shader));
            check(invocation.Arguments.SequenceEqual([tree.Entry, "--verb=waitforexitandrun", "--", tree.Proton,
                "waitforexitandrun", "C:\\windows\\system32\\start.exe", "/wait"]),
                "The container chain was wrong: " + string.Join(" | ", invocation.Arguments));
            check(invocation.Environment["STEAM_COMPAT_TOOL_PATHS"] == tree.Runtime + Path.PathSeparator + Path.GetDirectoryName(tree.Proton),
                "STEAM_COMPAT_TOOL_PATHS was wrong: " + invocation.Environment["STEAM_COMPAT_TOOL_PATHS"]);
            check(invocation.Environment["STEAM_COMPAT_LIBRARY_PATHS"].Split(Path.PathSeparator).Contains(tree.Steam),
                "STEAM_COMPAT_LIBRARY_PATHS did not name the Steam root.");
            check(invocation.Environment["STEAM_COMPAT_MOUNTS"].Split(Path.PathSeparator).Contains(LocalInstall.FullPath(game)) &&
                invocation.Environment["STEAM_COMPAT_MOUNTS"].Split(Path.PathSeparator).Contains(LocalInstall.FullPath(prefix)),
                "STEAM_COMPAT_MOUNTS did not name the game and prefix.");
            check(invocation.Environment["STEAM_COMPAT_SHADER_PATH"] == shader,
                "STEAM_COMPAT_SHADER_PATH was not passed.");

            var record = Path.Combine(root, "invocation-container.argv");
            var environment = Path.Combine(root, "invocation-container.env");
            var start = invocation.StartInfo(Path.GetDirectoryName(tree.Proton)!);
            start.Environment["HF_RECORD"] = record;
            start.Environment["HF_ENV"] = environment;
            using var process = Process.Start(start) ?? throw new InvalidDataException("The fake entry point did not start.");
            process.WaitForExit();
            check(process.ExitCode == 0, "The fake entry point failed: " + process.ExitCode);
            // "$@" in the fake entry point is everything after the program itself.
            check(File.ReadAllLines(record).SequenceEqual(invocation.Arguments.Skip(1)), "The entry point received other argv.");
            check(File.ReadAllText(environment).Trim() == invocation.Environment["STEAM_COMPAT_TOOL_PATHS"],
                "The entry point did not receive STEAM_COMPAT_TOOL_PATHS.");
        });

        test("The bash resolver keeps the container layer, writes its mode metadata and never starts it when disabled", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: bash runtime mode)"); return; }
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-steam.sh");
            var tree = ContainerTree(Path.Combine(root, "invocation-bash-mode"));
            var temp = Path.Combine(root, "invocation-bash-mode-temp");
            var command = """
                source "$1"
                hf_runtime_resolve "$2" "$3"
                hf_runtime_export
                printf 'mode=%s\n' "$HF_RUNTIME_MODE"
                hf_runtime_entrypoint_arguments run /bin/true
                printf 'first=%s\n' "${HF_RUNTIME_ARGV[0]}"
                hf_runtime_write_mode_file "$4"
                """;
            var bash = RunBash(command, helper, tree.Proton, tree.Steam, temp);
            check(bash.ExitCode == 0, "The bash runtime resolver failed: " + bash.Error);
            var lines = bash.Output.Replace("\r", "").Split('\n', StringSplitOptions.RemoveEmptyEntries);
            check(lines.Contains("mode=container") && lines.Contains("first=" + tree.Entry),
                "The self-test layer did not use the entry point: " + bash.Output);
            check(File.Exists(Path.Combine(temp, LinuxSandboxReadiness.ModeFileName)),
                "The wrapper did not write the self-test mode file.");
            check(File.ReadAllText(Path.Combine(temp, LinuxSandboxReadiness.ModeFileName)).Contains("\"mode\":\"container\"", StringComparison.Ordinal),
                "The self-test mode file did not say container.");

            var off = RunBash("""
                export HELLFORGE_STEAM_RUNTIME=off
                source "$1"
                hf_runtime_resolve "$2" "$3"
                hf_runtime_arguments run /bin/true
                printf '%s\n' "${HF_RUNTIME_ARGV[0]}"
                """, helper, tree.Proton, tree.Steam);
            check(off.ExitCode == 0 && off.Output.Trim() == tree.Proton,
                "HELLFORGE_STEAM_RUNTIME=off did not keep Proton on the host: " + off.Output + off.Error);

            var hostFolder = Path.Combine(root, "invocation-bash-host", "steamapps", "common", "Proton - Experimental");
            var hostProton = MakeProton(hostFolder, "/proton %verb%", null);
            var host = RunBash("""
                source "$1"
                hf_runtime_resolve "$2" "$3"
                hf_runtime_mode_line
                """, helper, hostProton, Path.Combine(root, "invocation-bash-host"));
            check(host.ExitCode == 0 &&
                host.Output.Split('\n').Count(line => line.Contains("no require_tool_appid", StringComparison.Ordinal)) == 1,
                "The host fallback reason did not appear on exactly one line: " + host.Output);
        });

        test("The runtime resolver's argv is the same from C# and the bash helper", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: bash invocation parity)"); return; }
            var tree = ContainerTree(Path.Combine(root, "invocation-parity"));
            var expected = LinuxProtonInvocation.Create(tree.Proton, tree.Steam, Path.Combine(root, "invocation-parity-prefix"),
                "waitforexitandrun", ["C:\\windows\\system32\\start.exe", "/wait"]);
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-steam.sh");
            var command = """
                source "$1"
                hf_runtime_resolve "$2" "$3"
                hf_runtime_arguments waitforexitandrun "$4" /wait
                printf '%s\n' "${HF_RUNTIME_ARGV[@]}"
                """;
            var bash = RunBash(command, helper, tree.Proton, tree.Steam, "C:\\windows\\system32\\start.exe");
            check(bash.ExitCode == 0, "The bash resolver failed: " + bash.Error);
            check(bash.Output.Replace("\r", "").Split('\n', StringSplitOptions.RemoveEmptyEntries).SequenceEqual(expected.Arguments),
                "The bash argv differs from C#: " + bash.Output);
        });
    }

    [System.Runtime.Versioning.SupportedOSPlatform("linux")]
    static string MakeProton(string folder, string commandline, string? appId)
    {
        Directory.CreateDirectory(folder);
        var proton = Path.Combine(folder, "proton");
        File.WriteAllText(proton, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"${HF_RECORD:?}\"\nprintf '%s\\n' \"${STEAM_COMPAT_TOOL_PATHS:-}\" > \"${HF_ENV:?}\"\n");
        File.SetUnixFileMode(proton, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        MakeManifest(folder, commandline, appId);
        return LocalInstall.FullPath(proton);
    }

    static void MakeManifest(string folder, string commandline, string? appId)
    {
        var text = "\"manifest\"\n{\n\t\"version\" \"2\"\n\t\"commandline\" \"" + commandline + "\"\n";
        if (!string.IsNullOrEmpty(appId)) text += "\t\"require_tool_appid\" \"" + appId + "\"\n";
        text += "}\n";
        File.WriteAllText(Path.Combine(folder, "toolmanifest.vdf"), text);
    }

    [System.Runtime.Versioning.SupportedOSPlatform("linux")]
    static (string Steam, string Proton, string Runtime, string Entry) ContainerTree(string sandbox)
    {
        var steam = Path.Combine(sandbox, "Steam");
        var library = Path.Combine(sandbox, "Library2");
        var protonFolder = Path.Combine(steam, "steamapps", "common", "Proton - Experimental");
        var proton = MakeProton(protonFolder, "/proton %verb%", "4183110");
        var runtime = Path.Combine(library, "steamapps", "common", "SteamLinuxRuntime_4");
        Directory.CreateDirectory(runtime);
        var entry = Path.Combine(runtime, "_v2-entry-point");
        File.WriteAllText(entry, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"${HF_RECORD:?}\"\nprintf '%s\\n' \"${STEAM_COMPAT_TOOL_PATHS:-}\" > \"${HF_ENV:?}\"\n");
        File.SetUnixFileMode(entry, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        MakeManifest(runtime, "/_v2-entry-point --verb=%verb% --", null);
        File.WriteAllText(Path.Combine(runtime, "VERSIONS.txt"),
            "#Name\tVersion\t\tRuntime\tRuntime_Version\tComment\ndepot\t4.0.test\t\t\t# Overall version number\nsteamrt4\t4.0.test\tsteamrt4\t4.0.test\t\n");
        Directory.CreateDirectory(Path.Combine(library, "steamapps"));
        File.WriteAllText(Path.Combine(library, "steamapps", "appmanifest_4183110.acf"),
            "\"AppState\" { \"appid\" \"4183110\" \"StateFlags\" \"4\" \"installdir\" \"SteamLinuxRuntime_4\" \"buildid\" \"1\" }\n");
        Directory.CreateDirectory(Path.Combine(steam, "steamapps"));
        File.WriteAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
            "\"libraryfolders\" { \"0\" { \"path\" \"" + steam + "\" } \"1\" { \"path\" \"" + library + "\" } }\n");
        return (steam, proton, LocalInstall.ResolveLinks(runtime), LocalInstall.ResolveLinks(entry));
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

    static string RepositoryRoot()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        return Environment.CurrentDirectory;
    }
}
