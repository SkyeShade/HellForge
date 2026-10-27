using System.Diagnostics;
using System.Text.Json;
using HellForge.Core;

// What a launcher of its own needs (docs/external-launchers.md): start and stop sessions as HellForge.exe's PLAY does,
// read an installation's state, its profiles and its mods, and change the mods as the Mods page does. Every safety step
// stays in the engine's session runner; these commands only start it, with the choices the launcher would pass.
static class Launch
{
    // One JSON object per line (play --wait, stop --wait), for a program reading as the session goes.
    static readonly JsonSerializerOptions Line = new(CopyPreparer.JsonOptions) { WriteIndented = false };
    static readonly string[] Ended = ["observed", "failed", "cleanup_failed"];

    // --install, else the installation this HellForge.Cli is in (app/cli).
    public static InstallLayout Install(Dictionary<string, string> options)
    {
        var install = options.TryGetValue("--install", out var root) ? new InstallLayout(root)
            : InstallLayout.Find(AppContext.BaseDirectory) ?? throw new ArgumentException("Missing --install: this HellForge.Cli is not in an installation.");
        if (!install.Exists) throw new InvalidDataException($"{install.Root} is not a HellForge installation.");
        return install;
    }

    static bool Yes(Dictionary<string, string> options, string key, bool saved) => options.GetValueOrDefault(key) switch
    {
        null => saved,
        "yes" => true,
        "no" => false,
        _ => throw new ArgumentException($"{key} takes yes or no.")
    };

    // A session runner of any installation: the engine allows one game at a time.
    static bool SessionRunning()
    {
        var found = Process.GetProcessesByName("HellForge.Session");
        foreach (var process in found) process.Dispose();
        return found.Length > 0;
    }

    static void Emit(object value) => Console.WriteLine(JsonSerializer.Serialize(value, Line));

    // play: the launcher's saved choices (launcher-settings.json), each replaced by its option when given.
    public static int Play(Dictionary<string, string> options)
    {
        if (!SessionRunner.IsSupported) throw new InvalidDataException(SessionRunner.UnsupportedMessage);
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        foreach (var (part, file) in new[] { ("The local backend", paths.Backend), ("The session runner", paths.Session) })
            if (!File.Exists(file)) throw new InvalidDataException($"{part} is missing ({file}): reinstall HellForge.");
        if (!SupportedBuild.Current.IsComplete(install.Game))
            throw new InvalidDataException("The game copy is not complete: open HellForge.exe once to set up (or repair) the installation.");
        var war = options.GetValueOrDefault("--war") ?? ProfileStore.LoadWar(paths);
        if (war is not (ProfileStore.WarLive or ProfileStore.WarFrozen or ProfileStore.WarOpen))
            throw new ArgumentException($"--war takes {ProfileStore.WarLive}, {ProfileStore.WarFrozen} or {ProfileStore.WarOpen}.");
        var session = new SessionOptions(Yes(options, "--local-playfab", ProfileStore.LoadLocalPlayFab(paths)),
            Yes(options, "--multiplayer", ProfileStore.LoadSdkShims(paths)), war,
            Yes(options, "--engine-log", ProfileStore.LoadEngineLog(paths)), Yes(options, "--crash-dump", ProfileStore.LoadCrashDump(paths)),
            GameLaunchOptions.Parse(options.TryGetValue("--game-options", out var typed) ? typed : ProfileStore.LoadLaunchOptions(paths)),
            Sandbox: Yes(options, "--sandbox", SessionRunner.SandboxByDefault(paths)), FullOwnershipCheck: Yes(options, "--full-ownership-check", false));
        var profiles = ProfileStore.List(paths);
        var name = options.GetValueOrDefault("--profile") ?? ProfileStore.LoadSelected(paths);
        var profile = profiles.FirstOrDefault(p => p.Name == name) ?? throw new ArgumentException(
            $"No profile named {name}; there are: {string.Join(", ", profiles.Select(p => p.Name))} (profiles --create makes one).");
        var arguments = SessionRunner.Arguments(paths, profile, session);
        if (Yes(options, "--dry-run", false))
        {
            Emit(new { @event = "dry-run", sessionRunner = paths.Session, arguments, crashDump = session.CrashDump });
            return 0;
        }
        if (SessionRunning()) throw new InvalidDataException("A HellForge session is running already: stop it first (stop).");

        // as the launcher does between sessions: only the newest session logs stay
        try { HellForge.Core.SessionLogs.Prune(paths.RunsFolder); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        var runner = new SessionRunner();
        var startedUtc = DateTime.UtcNow;
        try { runner.Start(paths, profile, session); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or PlatformNotSupportedException)
        {
            throw new InvalidDataException("Could not start the session runner: " + error.Message);
        }
        string? run;
        while ((run = SessionRunner.RunSince(paths, startedUtc)) == null)
        {
            if (!runner.IsRunning) throw new InvalidDataException("The session runner exited before starting: " + RunnerLog(install, startedUtc));
            Thread.Sleep(250);
        }
        Emit(new { @event = "started", runId = Path.GetFileName(run), run, pid = runner.ProcessId, profile = profile.Name, war, arguments });
        return Yes(options, "--wait", false) ? Follow(run, () => runner.IsRunning) : 0;
    }

    // Why the session runner stopped before it made its run folder: its last line in logs/session-runner.log, if new.
    static string RunnerLog(InstallLayout install, DateTime sinceUtc)
    {
        var log = Path.Combine(install.Logs, "session-runner.log");
        try
        {
            if (File.Exists(log) && File.GetLastWriteTimeUtc(log) >= sinceUtc.AddSeconds(-1))
                return File.ReadLines(log).LastOrDefault(l => l.Length > 0) ?? "";
        }
        catch (IOException) { }
        return "see " + log;
    }

    // Reports the run's status (result.json) as it changes, until the session runner has exited. 0 when the session
    // ended normally.
    static int Follow(string run, Func<bool> running)
    {
        var result = Path.Combine(run, "result.json");
        var last = "";
        var problemsShown = false;
        while (true)
        {
            var alive = running();
            if (SessionFiles.ReadJson(result) is { } state)
            {
                problemsShown = problemsShown || EmitProblems(state);
                last = EmitStatus(state, last);
            }
            if (!alive) break;
            Thread.Sleep(1000);
        }
        Emit(new { @event = "ended", status = last, result, logs = run });
        return last == "observed" ? 0 : 1;
    }

    // The mods' problems, once result.json lists them (an empty list prints nothing): true when it does.
    static bool EmitProblems(JsonElement state)
    {
        if (!(state.TryGetProperty("mods", out var mods) && mods.ValueKind == JsonValueKind.Object &&
            mods.TryGetProperty("problems", out var problems) && problems.ValueKind == JsonValueKind.Array))
            return false;
        var list = problems.EnumerateArray().Where(p => p.ValueKind == JsonValueKind.String).Select(p => p.GetString()).ToList();
        if (list.Count > 0) Emit(new { @event = "mod-problems", problems = list });
        return true;
    }

    // The run's status when it changed since the last one reported; returns the status reported last.
    static string EmitStatus(JsonElement state, string last)
    {
        var status = SessionFiles.Text(state, "status");
        if (status.Length == 0 || status == last) return last;
        var failure = SessionFiles.Text(state, "failure");
        Emit(new { @event = "status", status, failure = failure.Length > 0 ? failure : null });
        return status;
    }

    // stop: asks the session runner to close the game and clean up, as the launcher's STOP does.
    public static int Stop(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var run = options.TryGetValue("--run", out var given) ? Path.GetFullPath(given)
            : SessionRunner.LatestRun(paths) ?? throw new InvalidDataException("No session has run in this installation.");
        if (!LocalInstall.Within(run, install.Sessions) || !Directory.Exists(run))
            throw new InvalidDataException($"{run} is not a session of this installation.");
        if (!SessionRunning() || (SessionFiles.ReadJson(Path.Combine(run, "result.json")) is { } state && Ended.Contains(SessionFiles.Text(state, "status"))))
            throw new InvalidDataException("That session is not running.");
        SessionRunner.SignalStop(run);
        Emit(new { @event = "stopping", run });
        return Yes(options, "--wait", false) ? Follow(run, SessionRunning) : 0;
    }

    // status: what a launcher shows before PLAY.
    public static object Status(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var engine = SessionFiles.ReadJson(Path.Combine(install.App, "engine.json"));
        var latest = SessionRunner.LatestRun(paths);
        var state = latest != null ? SessionFiles.ReadJson(Path.Combine(latest, "result.json")) : null;
        return new
        {
            install = install.Root,
            game = install.Game,
            gameBuild = install.ReadMarker().BuildId,
            hellforge = AppVersion.Text,
            engine = engine is { } e ? SessionFiles.Text(e, "version") : null,
            modSdk = ModCatalog.SdkVersion,
            ready = new
            {
                gameCopy = SupportedBuild.Current.IsComplete(install.Game),
                sessionRunner = File.Exists(paths.Session),
                linuxNamespace = LinuxNetworkBlock.Requested
            },
            // how the copy is kept offline: the WFP filters or the Linux namespace setup placed, else the Windows Firewall rules,
            // which the session runner checks before each session and installs when missing (one Windows prompt)
            networkBlock = LinuxNetworkBlock.MarkerIsEmulated(install.Root) ? "linux-netns"
                : File.Exists(LinuxNetworkBlock.MarkerPath(install.Root)) ? "wfp" : "firewall",
            settings = new
            {
                profile = ProfileStore.LoadSelected(paths),
                war = ProfileStore.LoadWar(paths),
                localPlayFab = ProfileStore.LoadLocalPlayFab(paths),
                multiplayer = ProfileStore.LoadSdkShims(paths),
                engineLog = ProfileStore.LoadEngineLog(paths),
                crashDump = ProfileStore.LoadCrashDump(paths),
                sandbox = ProfileStore.LoadSandbox(paths),
                gameOptions = ProfileStore.LoadLaunchOptions(paths)
            },
            session = new
            {
                running = SessionRunning(),
                lastRun = latest,
                lastStatus = state is { } s ? SessionFiles.Text(s, "status") : null,
                lastFailure = state is { } f && SessionFiles.Text(f, "failure") is { Length: > 0 } failure ? failure : null
            },
            // the language packs in the copy (languages: the ones pinned, and how to add them); English is always there
            languages = SupportedLanguages.Current.InCopy(install.Game).Select(l => l.Name),
            logs = install.Logs,
            mods = install.Mods
        };
    }

    // profiles: the launcher's profiles and templates; --create <name> --template <template> makes one.
    public static object Profiles(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var templates = ProfileStore.LoadTemplates(paths);
        if (options.TryGetValue("--create", out var name))
        {
            var template = options.GetValueOrDefault("--template") ?? throw new ArgumentException("Missing --template.");
            if (templates.All(t => t.Name != template))
                throw new ArgumentException($"No template named {template}; there are: {string.Join(", ", templates.Select(t => t.Name))}.");
            if (ProfileStore.Create(paths, name, template) is { } error) throw new ArgumentException(error);
        }
        else if (options.ContainsKey("--template")) throw new ArgumentException("--template goes with --create.");
        var selected = ProfileStore.LoadSelected(paths);
        return new
        {
            profiles = ProfileStore.List(paths).Select(p => new
            {
                p.Name, p.Template, p.Folder, used = File.Exists(p.ProfileFile), selected = p.Name == selected
            }),
            templates = templates.Select(t => new { t.Name, t.Description, items = t.ItemCount })
        };
    }

    // fetch-war: the live galactic war into the cache a live-war session serves. A Linux session has no network, so
    // linux/hellforge-linux.sh fetches it before entering the namespace; on Windows the session refreshes it itself.
    public static object FetchWar(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        if (!File.Exists(paths.Backend)) throw new InvalidDataException($"The local backend is missing ({paths.Backend}): reinstall HellForge.");
        var start = new ProcessStartInfo(paths.Backend) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        start.ArgumentList.Add("fetch-live-war");
        start.ArgumentList.Add(paths.LiveWarCache);
        using var backend = Process.Start(start) ?? throw new InvalidDataException("The local backend did not start.");
        var output = backend.StandardOutput.ReadToEndAsync();
        var error = backend.StandardError.ReadToEndAsync();
        if (!backend.WaitForExit(TimeSpan.FromSeconds(90)))
        {
            backend.Kill();
            throw new InvalidDataException("The live war did not arrive within 90 s; a session serves the last one fetched.");
        }
        if (backend.ExitCode != 0)
            throw new InvalidDataException("Could not fetch the live war: " + (error.Result + output.Result).Trim());
        return new { fetched = true, cache = paths.LiveWarCache };
    }

    // mods: the Mods page's list, top first (the bottom one loads first; gameplay mods, then cosmetic ones).
    public static object Mods(Dictionary<string, string> options)
    {
        var install = Install(options);
        var (mods, problems) = ModCatalog.Load(install.Mods);
        var active = ModCatalog.Active(mods);
        var gameplay = active.Count(m => m.Gameplay && m.Id != ModCatalog.LocalStatsId);
        return new
        {
            mods = mods.Select(m => new
            {
                m.Id, m.Name, m.Version, m.Description, m.Enabled,
                group = m.Cosmetic ? "cosmetic" : "gameplay",
                tags = Tags(m),
                m.Folder, archive = m.Archive != null ? Path.Combine(install.Mods, m.Archive) : null,
                removable = Removable(m),
                // a cosmetic pack's options as its picker shows them (hellforge-mods.json "packs": "off" and the "sub" picks)
                options = m.Pack?.Options.Select(o => new
                {
                    o.Name, on = !m.PackChoice.Off.Contains(o.Name),
                    variants = o.SubOptions.Count > 0 ? o.SubOptions.Select(s => s.Name) : null,
                    picked = m.PackChoice.Sub.GetValueOrDefault(o.Name)
                })
            }),
            problems,
            // what friends compare in co-op (the launcher's "Modlist code"), or null when no gameplay mod is on
            modlist = gameplay > 0 ? ModCatalog.Digest(ModCatalog.LoadOrder(active))[..8] : null,
            settings = Path.Combine(install.Mods, ModSettings.FileName)
        };
    }

    static List<string> Tags(ModManifest mod)
    {
        var tags = new List<string>();
        if (mod.HasCode) tags.Add(mod.HasSource ? "source" : "code");
        if (mod.Stats.Count > 0) tags.Add("stats");
        if (mod.DataFiles.Count > 0 || mod.HasDataSpec) tags.Add("data");
        if (mod.Pack != null) tags.Add("pack");
        if (mod.Lua) tags.Add("lua");
        if (BundledMods.ShippedFiles(mod.Folder) != null) tags.Add("bundled");
        if (mod.Id == ModCatalog.LocalStatsId) tags.Add("local-stats");
        return tags;
    }

    // The player's own mods; the ones HellForge ships come back by themselves, so they are switched off instead.
    static bool Removable(ModManifest mod) => mod.Id != ModCatalog.LocalStatsId && BundledMods.ShippedFiles(mod.Folder) == null;

    // Mod changes wait for the session to end, as on the Mods page: a session stages the mods it started with.
    static void NoSession()
    {
        if (SessionRunning()) throw new InvalidDataException("A HellForge session is running: change mods once it has ended.");
    }

    static ModManifest Find(InstallLayout install, string id) =>
        ModCatalog.Load(install.Mods).Mods.FirstOrDefault(m => m.Id == id) ?? throw new ArgumentException($"No mod with the id {id} in {install.Mods}.");

    // mod-add: a mod's .zip (.rar, .7z), a folder mod's hellforge-mod.json or a cosmetic pack's manifest.json, copied into
    // mods/ as the Mods page's Add mods... does; a mod with the same id is replaced (an update).
    public static object ModAdd(Dictionary<string, string> options, string file)
    {
        var install = Install(options);
        NoSession();
        var (candidates, problems) = ModInstaller.Inspect(install, [file]);
        if (problems.Count > 0) throw new InvalidDataException(string.Join("; ", problems));
        var mod = candidates.Single();
        var added = ModInstaller.Add(install, mod);
        return new { mod.Id, mod.Name, mod.Version, installed = added, replaced = mod.Replaces };
    }

    // languages: the language packs pinned for the build, which are in the game copy and which Steam has installed (the
    // ones worth adding), each with the download_depot command that fetches it at the pinned manifest.
    public static object Languages(Dictionary<string, string> options)
    {
        var install = Install(options);
        var list = SupportedLanguages.Current;
        var game = SteamLocator.FindGame(options.GetValueOrDefault("--steam-root"));
        var installed = game != null ? list.InstalledIn(game.ManifestPath).Select(l => l.Name).ToHashSet() : new HashSet<string>();
        var inCopy = list.InCopy(install.Game).Select(l => l.Name).ToHashSet();
        return new
        {
            buildId = list.BuildId,
            languages = list.Languages.Select(l => new
            {
                name = l.Name,
                dlcAppId = l.DlcAppId,
                bytes = l.TotalBytes,
                inCopy = inCopy.Contains(l.Name),
                installedInSteam = installed.Contains(l.Name),
                command = SteamDepotDownload.Commands(l.AsBuild(SupportedBuild.Current)).Single()
            })
        };
    }

    // language-add: copies one pack from Steam's download of it (its download_depot command) into the game copy, every
    // file hashed against the pinned list, then removes the download unless --keep yes.
    public static object LanguageAdd(Dictionary<string, string> options, string name)
    {
        var install = Install(options);
        NoSession();
        var language = SupportedLanguages.Current.Find(name)
            ?? throw new ArgumentException($"No language pack {name} is pinned for build {SupportedBuild.Current.BuildId}.");
        var steamRoot = options.GetValueOrDefault("--steam-root") ?? SteamLocator.SteamRoot()
            ?? throw new ArgumentException("Missing --steam-root: Steam was not found.");
        var pack = language.AsBuild(SupportedBuild.Current);
        if (!SteamDepotDownload.IsComplete(steamRoot, pack))
            throw new InvalidDataException($"Steam has not downloaded the {name} pack yet; run {SteamDepotDownload.Commands(pack).Single()} in Steam's console.");
        GameCopier.CopyFromDepots(steamRoot, install.Game, pack);
        if (!Yes(options, "--keep", false)) SteamDepotDownload.Delete(steamRoot, pack);
        return new { name, files = language.Files.Count, bytes = language.TotalBytes, inCopy = pack.IsComplete(install.Game) };
    }

    public static object ModRemove(Dictionary<string, string> options, string id)
    {
        var install = Install(options);
        NoSession();
        var mod = Find(install, id);
        if (!Removable(mod)) throw new InvalidDataException($"{id} ships with HellForge: switch it off instead (mod-switch).");
        ModInstaller.Remove(install, mod);
        return new { mod.Id, removed = mod.Archive != null ? Path.Combine(install.Mods, mod.Archive) : mod.Folder };
    }

    public static object ModSwitch(Dictionary<string, string> options, string id, string on)
    {
        var install = Install(options);
        NoSession();
        var enabled = on switch { "yes" => true, "no" => false, _ => throw new ArgumentException("--on takes yes or no.") };
        Find(install, id);
        if (id == ModCatalog.LocalStatsId && !enabled) throw new InvalidDataException("Local Stats is always on: it holds the in-game editor's saved changes (reset them in the editor).");
        ModSettings.SetEnabled(install.Mods, id, enabled);
        return new { id, enabled };
    }

    // mod-pack: a cosmetic pack's options as its picker sets them (hellforge-mods.json "packs"): --option <name> --on
    // yes | no switches one option, --variant <name> picks one of its variants; --all yes | no switches every option.
    public static object ModPack(Dictionary<string, string> options, string id)
    {
        var install = Install(options);
        NoSession();
        var mod = Find(install, id);
        var pack = mod.Pack ?? throw new ArgumentException($"{id} is not a cosmetic pack with options.");
        var off = mod.PackChoice.Off.ToHashSet(StringComparer.Ordinal);
        var sub = new Dictionary<string, string>(mod.PackChoice.Sub, StringComparer.Ordinal);
        if (options.ContainsKey("--all")) PickAll(options, pack, off);
        else PickOption(options, id, pack, off, sub);
        ModSettings.SetChoice(install.Mods, id, new CosmeticPacks.Choice(off, sub));
        var saved = Find(install, id);
        return new
        {
            id,
            options = pack.Options.Select(o => new
            {
                o.Name, on = !saved.PackChoice.Off.Contains(o.Name),
                variants = o.SubOptions.Count > 0 ? o.SubOptions.Select(s => s.Name) : null,
                picked = saved.PackChoice.Sub.GetValueOrDefault(o.Name)
            })
        };
    }

    // mod-pack --all: every option on or off.
    static void PickAll(Dictionary<string, string> options, CosmeticPacks.Pack pack, HashSet<string> off)
    {
        if (options.ContainsKey("--option")) throw new ArgumentException("--all goes without --option.");
        if (Yes(options, "--all", true)) off.Clear();
        else off.UnionWith(pack.Options.Select(o => o.Name));
    }

    // mod-pack --option: that option on or off (--on), its variant (--variant), or both.
    static void PickOption(Dictionary<string, string> options, string id, CosmeticPacks.Pack pack, HashSet<string> off, Dictionary<string, string> sub)
    {
        var name = options.GetValueOrDefault("--option") ?? throw new ArgumentException("Missing --option (or --all).");
        var option = pack.Options.FirstOrDefault(o => o.Name == name) ?? throw new ArgumentException(
            $"{id} has no option named {name}; it has: {string.Join(", ", pack.Options.Select(o => o.Name))}.");
        if (options.ContainsKey("--on"))
        {
            if (Yes(options, "--on", true)) off.Remove(name);
            else off.Add(name);
        }
        if (options.TryGetValue("--variant", out var variant))
        {
            if (option.SubOptions.All(s => s.Name != variant)) throw new ArgumentException(option.SubOptions.Count == 0
                ? $"{name} has no variants." : $"{name} has no variant named {variant}; it has: {string.Join(", ", option.SubOptions.Select(s => s.Name))}.");
            sub[name] = variant;
        }
        if (!options.ContainsKey("--on") && !options.ContainsKey("--variant")) throw new ArgumentException("Missing --on or --variant.");
    }

    // settings: the launcher's saved choices (launcher-settings.json), which play and HellForge.exe's PLAY both start from.
    // Each option given is saved first; all of them are printed. They apply from the next session.
    public static object Settings(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        if (options.TryGetValue("--profile", out var name))
        {
            var profiles = ProfileStore.List(paths);
            if (profiles.All(p => p.Name != name)) throw new ArgumentException(
                $"No profile named {name}; there are: {string.Join(", ", profiles.Select(p => p.Name))} (profiles --create makes one).");
            ProfileStore.SaveSelected(paths, name);
        }
        if (options.TryGetValue("--war", out var war))
        {
            if (war is not (ProfileStore.WarLive or ProfileStore.WarFrozen or ProfileStore.WarOpen))
                throw new ArgumentException($"--war takes {ProfileStore.WarLive}, {ProfileStore.WarFrozen} or {ProfileStore.WarOpen}.");
            ProfileStore.SaveWar(paths, war);
        }
        if (options.ContainsKey("--local-playfab")) ProfileStore.SaveLocalPlayFab(paths, Yes(options, "--local-playfab", true));
        if (options.ContainsKey("--multiplayer")) ProfileStore.SaveSdkShims(paths, Yes(options, "--multiplayer", true));
        if (options.ContainsKey("--engine-log")) ProfileStore.SaveEngineLog(paths, Yes(options, "--engine-log", true));
        if (options.ContainsKey("--crash-dump")) ProfileStore.SaveCrashDump(paths, Yes(options, "--crash-dump", false));
        if (options.ContainsKey("--sandbox")) ProfileStore.SaveSandbox(paths, Yes(options, "--sandbox", false));
        if (options.TryGetValue("--game-options", out var typed))
        {
            _ = GameLaunchOptions.Parse(typed); // refused as play refuses it: never a switch the session sets itself
            ProfileStore.SaveLaunchOptions(paths, typed);
        }
        // Play with strangers is the mods settings' (hellforge-mods.json), which the backend reads, even during a session.
        if (options.ContainsKey("--strangers")) ModSettings.SetPlayWithStrangers(install.Mods, Yes(options, "--strangers", false));
        return new
        {
            profile = ProfileStore.LoadSelected(paths),
            war = ProfileStore.LoadWar(paths),
            localPlayFab = ProfileStore.LoadLocalPlayFab(paths),
            multiplayer = ProfileStore.LoadSdkShims(paths),
            engineLog = ProfileStore.LoadEngineLog(paths),
            crashDump = ProfileStore.LoadCrashDump(paths),
            gameOptions = ProfileStore.LoadLaunchOptions(paths),
            sandbox = ProfileStore.LoadSandbox(paths),
            steamBridge = SessionRunner.HasSteamBridge(paths),
            playWithStrangers = ModSettings.PlayWithStrangers(install.Mods),
            file = paths.SettingsFile
        };
    }

    // logs: the sessions' run folders, newest first, with how each ended; --run <id> picks one and lists its files;
    // --export <file.zip> zips one (the newest without --run) as the launcher's Save logs to Desktop does.
    public static object Logs(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var runs = SessionLogs.Runs(paths.RunsFolder);
        SessionLogs.Run? picked = null;
        if (options.TryGetValue("--run", out var wanted))
            picked = runs.FirstOrDefault(r => r.Id == wanted || string.Equals(Path.GetFullPath(r.Folder), Path.GetFullPath(wanted), StringComparison.OrdinalIgnoreCase))
                ?? throw new ArgumentException($"No session {wanted} in {paths.RunsFolder}.");
        object Describe(SessionLogs.Run r) => new { r.Id, r.Folder, r.StartedUtc, r.Status, r.Failure, r.GameExitCode, r.CompletedUtc, crashed = CrashReport.Crashed(r.Folder) };
        if (options.TryGetValue("--crash-report", out var report)) return CrashReportZip(install, picked ?? runs.FirstOrDefault(), report, Yes(options, "--dump", false), Describe);
        if (options.ContainsKey("--dump")) throw new ArgumentException("--dump goes with --crash-report.");
        if (options.TryGetValue("--export", out var target))
        {
            var run = picked ?? runs.FirstOrDefault() ?? throw new InvalidDataException("No session logs yet: play a session first.");
            target = Path.GetFullPath(target);
            if (File.Exists(target)) throw new ArgumentException($"{target} exists already.");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            var exported = SessionLogs.Export(run.Folder, Path.Combine(install.UserData, "logs"), target);
            return new { run = Describe(run), exported.Zip, exported.Files, exported.Skipped };
        }
        var runnerLog = Path.Combine(install.Logs, "session-runner.log");
        return new
        {
            runs = runs.Select(Describe),
            files = picked == null ? null : Directory.EnumerateFiles(picked.Folder, "*", SearchOption.AllDirectories)
                .Select(f => new { path = Path.GetRelativePath(picked.Folder, f).Replace('\\', '/'), bytes = new FileInfo(f).Length }),
            sessionRunnerLog = File.Exists(runnerLog) ? runnerLog : null,
            gameLogs = Path.Combine(install.UserData, "logs")
        };
    }

    // logs --crash-report <file.zip> [--dump yes]: the crash report of one session (the newest without --run), as the
    // launcher's Save crash report makes it (CrashReport); the crash dump goes in only with --dump yes. Nothing is sent.
    static object CrashReportZip(InstallLayout install, SessionLogs.Run? run, string target, bool dump, Func<SessionLogs.Run, object> describe)
    {
        if (run == null) throw new InvalidDataException("No session logs yet: play a session first.");
        target = Path.GetFullPath(target);
        if (File.Exists(target)) throw new ArgumentException($"{target} exists already.");
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var saved = CrashReport.Save(run.Folder, Path.Combine(install.UserData, "logs"), target, dump);
        return new
        {
            run = describe(run), saved.Zip, saved.Files, saved.Skipped, dump = saved.Dump,
            dumpAvailable = CrashReport.Dump(run.Folder) != null, issuePage = CrashReport.IssuePage
        };
    }

    // mod-order: the listed mods first, top first, in that order; the others keep theirs after them. Gameplay mods stay
    // above cosmetic ones (ModCatalog.Ordered), so each group's order is what changes.
    public static object ModOrder(Dictionary<string, string> options, string ids)
    {
        var install = Install(options);
        NoSession();
        var listed = ids.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).ToList();
        var mods = ModCatalog.Load(install.Mods).Mods;
        foreach (var id in listed)
            if (mods.All(m => m.Id != id)) throw new ArgumentException($"No mod with the id {id} in {install.Mods}.");
        if (listed.Distinct(StringComparer.Ordinal).Count() != listed.Count) throw new ArgumentException("--ids lists a mod twice.");
        ModSettings.SetOrder(install.Mods, listed.Concat(mods.Select(m => m.Id).Where(id => !listed.Contains(id))).ToList());
        return new { order = ModCatalog.Load(install.Mods).Mods.Select(m => m.Id) };
    }
}
