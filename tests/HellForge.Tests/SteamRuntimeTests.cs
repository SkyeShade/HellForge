using System.Diagnostics;
using HellForge.Core;

// J.3: Steam's two-manifest Steam Linux Runtime chain in SteamRuntimeLocator and linux/hf-steam.sh. Every fixture is a
// fake tree; no Steam, Proton or container is started.
static class SteamRuntimeTests
{
    const string TableVersion = "4.0.20261021.265997";

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        _ = reject;
        test("Steam runtime resolver follows the Proton manifest into a second library with the exact argv", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Steam runtime resolver)"); return; }
            var (steam, proton, runtime) = MainTree(Path.Combine(root, "steam-runtime-main"));
            var resolved = SteamRuntimeLocator.Resolve(proton, steam);
            CheckTree(check, resolved, proton, runtime);
            check(resolved.Name == "steamrt4" && resolved.Version == TableVersion,
                "The VERSIONS table was not read: " + resolved.Name + " " + resolved.Version);
            var versions = Path.Combine(runtime, "VERSIONS.txt");
            check(resolved.VersionsSha256 == LocalInstall.Hash(versions), "The VERSIONS.txt hash is wrong.");
            check(resolved.Identity == runtime + "|" + LocalInstall.Hash(versions), "The readiness identity is wrong.");
            var run = SteamRuntimeLocator.Arguments(resolved, proton, "run", ["C:\\game.exe"]);
            check(run.SequenceEqual([Path.Combine(runtime, "_v2-entry-point"), "--verb=run", "--", proton, "run", "C:\\game.exe"]),
                "The run argv is wrong: " + string.Join(" ", run));
            var wait = SteamRuntimeLocator.Arguments(resolved, proton, "waitforexitandrun", ["domain"]);
            check(wait[1] == "--verb=waitforexitandrun" && wait[4] == "waitforexitandrun", "The wait verb was not substituted.");
        });

        test("Steam runtime resolver parses the real nested toolmanifest shape", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: nested Steam manifests)"); return; }
            var sandbox = Path.Combine(root, "steam-runtime-real-manifests");
            var steam = Path.Combine(sandbox, "Steam");
            var library = Path.Combine(sandbox, "Library2");
            var protonFolder = Path.Combine(steam, "steamapps", "common", "Proton - Experimental");
            Directory.CreateDirectory(protonFolder);
            var proton = Path.Combine(protonFolder, "proton");
            File.WriteAllText(proton, "#!/bin/sh\n");
            SetMode(proton, writable: true);
            const string protonManifest = "\"manifest\"\n{\n\t\"version\" \"2\"\n\t\"commandline\" \"/proton %verb%\"\n\t\"require_tool_appid\" \"4183110\"\n}\n";
            const string runtimeManifest = "// Generated file, do not edit\n\"manifest\"\n{\n\t\"commandline\" \"/_v2-entry-point --verb=%verb% --\"\n\t\"filter_exclusive_priority\" \"4\"\n\t\"version\" \"2\"\n\t\"use_tool_subprocess_reaper\" \"1\"\n\t\"compatmanager_layer_name\" \"container-runtime\"\n}\n";
            File.WriteAllText(Path.Combine(protonFolder, "toolmanifest.vdf"), protonManifest);
            var parsed = ValveData.Parse(runtimeManifest).Children["manifest"];
            check(parsed.Required("commandline") == "/_v2-entry-point --verb=%verb% --" && parsed.Required("version") == "2",
                "ValveData did not parse the real runtime toolmanifest text.");
            var runtime = Path.Combine(library, "steamapps", "common", "SteamLinuxRuntime_4");
            Directory.CreateDirectory(runtime);
            var entry = Path.Combine(runtime, "_v2-entry-point");
            File.WriteAllText(entry, "#!/bin/sh\n");
            SetMode(entry);
            File.WriteAllText(Path.Combine(runtime, "toolmanifest.vdf"), runtimeManifest);
            File.WriteAllText(Path.Combine(runtime, "VERSIONS.txt"), VersionsTable());
            AppManifest(library, "4183110", "SteamLinuxRuntime_4");
            LibraryFolders(steam, library);
            var resolved = SteamRuntimeLocator.Resolve(proton, steam);
            check(resolved.Mode == SteamRuntimeMode.Container && resolved.AppId == "4183110" &&
                resolved.ProtonCommandLine.SequenceEqual(["/proton", "%verb%"]) &&
                resolved.RuntimeCommandLine.SequenceEqual(["/_v2-entry-point", "--verb=%verb%", "--"]),
                "The nested manifest shape was not parsed: " + resolved.Reason);
        });

        test("Steam runtime resolver reads a custom tool's own manifest and its extra token", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux custom Steam tool)"); return; }
            var sandbox = Path.Combine(root, "steam-runtime-custom");
            var steam = Path.Combine(sandbox, "Steam");
            var library = Path.Combine(sandbox, "Library2");
            var custom = Proton(Path.Combine(steam, "compatibilitytools.d", "GE-Proton10-25"), "1628350", "/proton --j2-inner %verb%");
            File.WriteAllText(Path.Combine(Path.GetDirectoryName(custom)!, "compatibilitytool.vdf"),
                "\"compatibilitytools\" { \"compat_tools\" { \"GE-Proton10-25\" { \"display_name\" \"GE-Proton\" \"install_path\" \".\" } } }");
            var runtime = Runtime(Path.Combine(library, "steamapps", "common", "SteamLinuxRuntime_sniper"),
                "/_v2-entry-point --verb=%verb% --", VersionsTable());
            AppManifest(library, "1628350", "SteamLinuxRuntime_sniper");
            LibraryFolders(steam, library);
            var resolved = SteamRuntimeLocator.Resolve(custom, steam);
            check(resolved.Mode == SteamRuntimeMode.Container && resolved.AppId == "1628350", "The custom tool did not resolve.");
            var argv = SteamRuntimeLocator.Arguments(resolved, custom, "run", ["C:\\game.exe"]);
            check(argv.SequenceEqual([Path.Combine(runtime, "_v2-entry-point"), "--verb=run", "--", custom, "--j2-inner", "run",
                "C:\\game.exe"]), "The custom tool argv is wrong: " + string.Join(" ", argv));
        });

        test("Steam runtime resolver falls back to host mode with a reason for every unsupported manifest", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux Steam runtime fallbacks)"); return; }
            var sandbox = Path.Combine(root, "steam-runtime-fallbacks");
            var steam = Path.Combine(sandbox, "Steam");
            var library = Path.Combine(sandbox, "Library2");
            var runtime = Runtime(Path.Combine(library, "steamapps", "common", "SteamLinuxRuntime_4"),
                "/_v2-entry-point --verb=%verb% --", VersionsTable());
            AppManifest(library, "4183110", "SteamLinuxRuntime_4");
            LibraryFolders(steam, library);

            var noAppId = Proton(Path.Combine(steam, "steamapps", "common", "Proton 9.0"), null, "/proton %verb%");
            var host = SteamRuntimeLocator.Resolve(noAppId, steam);
            check(host.Mode == SteamRuntimeMode.Host && host.Reason.Contains("require_tool_appid", StringComparison.Ordinal),
                "A Proton without require_tool_appid did not run on the host: " + host.Reason);
            check(SteamRuntimeLocator.Arguments(host, noAppId, "run", ["x"]).SequenceEqual([noAppId, "run", "x"]),
                "Host mode did not use the direct proton form.");

            var hostToken = Proton(Path.Combine(steam, "steamapps", "common", "Proton Host Custom"), null, "/proton --host-token %verb%");
            var token = SteamRuntimeLocator.Resolve(hostToken, steam);
            check(token.Mode == SteamRuntimeMode.Host &&
                SteamRuntimeLocator.Arguments(token, hostToken, "run", ["x"]).SequenceEqual([hostToken, "--host-token", "run", "x"]),
                "A host manifest's extra token was not kept.");

            var missing = Proton(Path.Combine(steam, "steamapps", "common", "Proton Missing"), "9999999", "/proton %verb%");
            var missingRuntime = SteamRuntimeLocator.Resolve(missing, steam);
            check(missingRuntime.Mode == SteamRuntimeMode.Host &&
                missingRuntime.Reason.Contains("9999999", StringComparison.Ordinal), "A missing runtime did not name its appid.");

            var broken = Proton(Path.Combine(steam, "steamapps", "common", "Proton Broken"), "4183111", "/proton %verb%");
            var brokenRuntime = Path.Combine(library, "steamapps", "common", "BrokenRuntime");
            Directory.CreateDirectory(brokenRuntime);
            File.WriteAllText(Path.Combine(brokenRuntime, "toolmanifest.vdf"), "not a manifest\n");
            AppManifest(library, "4183111", "BrokenRuntime");
            var brokenResult = SteamRuntimeLocator.Resolve(broken, steam);
            check(brokenResult.Mode == SteamRuntimeMode.Host &&
                brokenResult.Reason.Contains("toolmanifest", StringComparison.Ordinal), "A broken runtime manifest did not fall back.");

            var escape = Proton(Path.Combine(steam, "steamapps", "common", "Proton Escape"), "4183112", "/proton %verb%");
            var escapeRuntime = Path.Combine(library, "steamapps", "common", "EscapeRuntime");
            Directory.CreateDirectory(escapeRuntime);
            File.WriteAllText(Path.Combine(escapeRuntime, "toolmanifest.vdf"),
                "\"manifest\" { \"version\" \"2\" \"commandline\" \"/../escape %verb%\" }\n");
            AppManifest(library, "4183112", "EscapeRuntime");
            var escapeResult = SteamRuntimeLocator.Resolve(escape, steam);
            check(escapeResult.Mode == SteamRuntimeMode.Host &&
                escapeResult.Reason.Contains("traversal", StringComparison.Ordinal), "An escaping entry point was accepted.");

            var wrongInner = Proton(Path.Combine(steam, "steamapps", "common", "Proton Wrong Inner"), "4183110", "/elsewhere %verb%");
            var wrongResult = SteamRuntimeLocator.Resolve(wrongInner, steam);
            check(wrongResult.Mode == SteamRuntimeMode.Host &&
                wrongResult.Reason.Contains("usable commandline", StringComparison.Ordinal),
                "A Proton commandline naming another file was accepted: " + wrongResult.Reason);

            var traversal = Proton(Path.Combine(steam, "steamapps", "common", "Proton Traversal"), "4183113", "/proton %verb%");
            var traversalRuntime = Path.Combine(library, "steamapps", "common", "TraversalRuntime");
            Directory.CreateDirectory(traversalRuntime);
            File.WriteAllText(Path.Combine(traversalRuntime, "toolmanifest.vdf"),
                "\"manifest\" { \"version\" \"2\" \"commandline\" \"/_v2-entry-point --verb=%verb% --\" }\n");
            File.WriteAllText(Path.Combine(traversalRuntime, "_v2-entry-point"), "#!/bin/sh\n");
            SetMode(Path.Combine(traversalRuntime, "_v2-entry-point"));
            AppManifest(library, "4183113", "../outside");
            var traversalResult = SteamRuntimeLocator.Resolve(traversal, steam);
            check(traversalResult.Mode == SteamRuntimeMode.Host, "An installdir traversal was accepted.");

            var configured = Environment.GetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME");
            try
            {
                Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME", "off");
                var off = SteamRuntimeLocator.Resolve(Proton(Path.Combine(steam, "steamapps", "common", "Proton - Experimental"),
                    "4183110", "/proton %verb%"), steam);
                check(off.Mode == SteamRuntimeMode.Host && off.Reason == "HELLFORGE_STEAM_RUNTIME=off",
                    "HELLFORGE_STEAM_RUNTIME=off did not select host mode.");
            }
            finally { Environment.SetEnvironmentVariable("HELLFORGE_STEAM_RUNTIME", configured); }
            _ = runtime;
        });

        test("VERSIONS.txt is parsed as a table; unparseable and missing files are distinguished", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: VERSIONS parsing)"); return; }
            var (steam, proton, runtime) = MainTree(Path.Combine(root, "steam-runtime-versions"));
            var versions = Path.Combine(runtime, "VERSIONS.txt");
            File.WriteAllText(versions, "not a table\n");
            var unparseable = SteamRuntimeLocator.Resolve(proton, steam);
            check(unparseable.Mode == SteamRuntimeMode.Container && unparseable.VersionsReadable &&
                unparseable.Version == "unknown" && unparseable.Name == "SteamLinuxRuntime_4" && unparseable.VersionsSha256 != null,
                "A readable but unparseable VERSIONS.txt was not recorded as unknown.");
            File.Delete(versions);
            var missing = SteamRuntimeLocator.Resolve(proton, steam);
            check(missing.Mode == SteamRuntimeMode.Container && !missing.VersionsReadable &&
                missing.Version == "unknown" && missing.VersionsSha256 == null &&
                missing.Identity == runtime + "|no-hash", "A missing VERSIONS.txt was not recorded as unreadable.");
            File.WriteAllText(versions, "#Name\tVersion\t\tRuntime\tRuntime_Version\tComment\r\n" +
                "steamrt4\t4.0.test\tsteamrt4\t4.0.test\t\r\n" +
                "depot\t4.0.20261021.265997\t\t\t# Overall version number\r\n");
            var reordered = SteamRuntimeLocator.Resolve(proton, steam);
            check(reordered.Name == "steamrt4" && reordered.Version == TableVersion,
                "The table's runtime/depot rows were not read in either order.");
            var (name, version) = SteamRuntimeLocator.ParseVersionsText(
                "#Name\tVersion\t\tRuntime\nsteamrt4\t9.9\tsteamrt4\n", "fallback");
            check(name == "steamrt4" && version == "unknown", "The table parser invented a version without a depot row.");
        });

        test("The bash resolver returns the same mode, fields and argv as SteamRuntimeLocator", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: bash Steam runtime resolver)"); return; }
            var (steam, proton, runtime) = MainTree(Path.Combine(root, "steam-runtime-bash"));
            var expected = SteamRuntimeLocator.Resolve(proton, steam);
            var expectedArgv = SteamRuntimeLocator.Arguments(expected, proton, "run", ["C:\\game.exe"]);
            var helper = Path.Combine(RepositoryRoot(), "linux", "hf-steam.sh");
            check(File.Exists(helper), "linux/hf-steam.sh was not found: " + helper);
            const string command = """
                source "$1"
                hf_runtime_resolve "$2" "$3"
                printf '%s\n' "$HF_RUNTIME_MODE"
                printf '%s\n' "$HF_RUNTIME_APPID"
                printf '%s\n' "$HF_RUNTIME_NAME"
                printf '%s\n' "$HF_RUNTIME_VERSION"
                printf '%s\n' "$HF_RUNTIME_VERSIONS_SHA256"
                hf_runtime_arguments run "$4"
                printf '%s\n' "${HF_RUNTIME_ARGV[@]}"
                hf_runtime_mode_line
                """;
            var bash = RunBash(command, helper, proton, steam, "C:\\game.exe");
            check(bash.ExitCode == 0, "The bash resolver failed: " + bash.Error);
            var lines = bash.Output.Replace("\r", "").Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
            check(lines.Count == 5 + expectedArgv.Count + 1, "Unexpected bash resolver output:\n" + bash.Output);
            check(lines[0] == "container" && lines[1] == "4183110" && lines[2] == expected.Name && lines[3] == expected.Version &&
                lines[4] == expected.VersionsSha256, "The bash resolver fields differ: " + string.Join(" | ", lines.Take(5)));
            check(lines.Skip(5).Take(expectedArgv.Count).SequenceEqual(expectedArgv),
                "The bash argv differs: " + string.Join(" ", lines.Skip(5).Take(expectedArgv.Count)));
            check(lines[^1] == $"HellForge: Steam Linux Runtime: container \"{expected.Name}\" {expected.Version} at {runtime}; Proton runs inside it.",
                "The bash mode line differs: " + lines[^1]);
        });
    }

    static void CheckTree(Action<bool, string> check, SteamRuntime resolved, string proton, string runtime)
    {
        check(resolved.Mode == SteamRuntimeMode.Container, "The runtime was not selected: " + resolved.Reason);
        check(resolved.AppId == "4183110", "The runtime appid is wrong: " + resolved.AppId);
        check(resolved.Folder == runtime, "The runtime folder is wrong: " + resolved.Folder);
        check(resolved.EntryPoint == Path.Combine(runtime, "_v2-entry-point"), "The entry point is wrong: " + resolved.EntryPoint);
        check(resolved.ProtonCommandLine.SequenceEqual(["/proton", "%verb%"]), "The Proton commandline was lost.");
        check(resolved.RuntimeCommandLine.SequenceEqual(["/_v2-entry-point", "--verb=%verb%", "--"]),
            "The runtime commandline was lost.");
        check(resolved.Folder != null && LocalInstall.ResolveLinks(resolved.Folder) == runtime, "The runtime folder was not resolved.");
        _ = proton;
    }

    static (string Steam, string Proton, string Runtime) MainTree(string sandbox)
    {
        var steam = Path.Combine(sandbox, "Steam");
        var library = Path.Combine(sandbox, "Library2");
        var proton = Proton(Path.Combine(steam, "steamapps", "common", "Proton - Experimental"), "4183110", "/proton %verb%");
        var runtime = Runtime(Path.Combine(library, "steamapps", "common", "SteamLinuxRuntime_4"),
            "/_v2-entry-point --verb=%verb% --", VersionsTable());
        AppManifest(library, "4183110", "SteamLinuxRuntime_4");
        LibraryFolders(steam, library);
        return (steam, proton, runtime);
    }

    static string VersionsTable() =>
        "#Name\tVersion\t\tRuntime\tRuntime_Version\tComment\n" +
        $"depot\t{TableVersion}\t\t\t# Overall version number\n" +
        $"steamrt4\t{TableVersion}\tsteamrt4\t{TableVersion}\t\n";

    static void SetMode(string path, bool writable = false)
    {
        if (!OperatingSystem.IsLinux()) return;
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserExecute |
            (writable ? UnixFileMode.UserWrite : UnixFileMode.None));
    }

    static string Proton(string folder, string? appId, string commandline)
    {
        Directory.CreateDirectory(folder);
        var script = Path.Combine(folder, "proton");
        File.WriteAllText(script, "#!/bin/sh\n");
        SetMode(script, writable: true);
        Directory.CreateDirectory(Path.Combine(folder, "files"));
        var manifest = "\"manifest\"\n{\n  \"version\" \"2\"\n";
        if (commandline.Length > 0) manifest += $"  \"commandline\" \"{commandline}\"\n";
        if (!string.IsNullOrEmpty(appId)) manifest += $"  \"require_tool_appid\" \"{appId}\"\n";
        manifest += "}\n";
        File.WriteAllText(Path.Combine(folder, "toolmanifest.vdf"), manifest);
        return LocalInstall.FullPath(script);
    }

    static string Runtime(string folder, string commandline, string versions)
    {
        Directory.CreateDirectory(folder);
        var entry = Path.Combine(folder, "_v2-entry-point");
        File.WriteAllText(entry, "#!/bin/sh\n");
        SetMode(entry);
        File.WriteAllText(Path.Combine(folder, "toolmanifest.vdf"),
            $"\"manifest\"\n{{\n  \"version\" \"2\"\n  \"commandline\" \"{commandline}\"\n}}\n");
        File.WriteAllText(Path.Combine(folder, "VERSIONS.txt"), versions);
        return LocalInstall.ResolveLinks(folder);
    }

    static void AppManifest(string library, string appId, string installDir)
    {
        Directory.CreateDirectory(Path.Combine(library, "steamapps"));
        Directory.CreateDirectory(Path.Combine(library, "steamapps", "common", installDir));
        File.WriteAllText(Path.Combine(library, "steamapps", $"appmanifest_{appId}.acf"),
            $"\"AppState\" {{ \"appid\" \"{appId}\" \"StateFlags\" \"4\" \"installdir\" \"{installDir}\" \"buildid\" \"1\" }}\n");
    }

    static void LibraryFolders(string steam, string library)
    {
        Directory.CreateDirectory(Path.Combine(steam, "steamapps"));
        File.WriteAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
            "\"libraryfolders\" { \"0\" { \"path\" \"" + steam.Replace("\\", "\\\\") + "\" } \"1\" { \"path\" \"" +
            library.Replace("\\", "\\\\") + "\" } }\n");
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
