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

    // The launch checks that do not depend on any choice: platform, installed programs and the game copy.
    static (InstallLayout Install, InstallSessionPaths Paths) Preflight(Dictionary<string, string> options)
    {
        if (!SessionRunner.IsSupported) throw new InvalidDataException(SessionRunner.UnsupportedMessage);
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        foreach (var (part, file) in new[] { ("The local backend", paths.Backend), ("The session runner", paths.Session) })
            if (!File.Exists(file)) throw new InvalidDataException($"{part} is missing ({file}): reinstall HellForge.");
        // the SDK stand-ins a session cut short left in game/bin go back first, as when the launcher opens
        try { SdkShimRecovery.Recover(install); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
        if (SupportedBuild.Current.FirstCompletenessProblem(install.Game) is { } problem)
            throw new InvalidDataException($"The game copy is not complete ({problem}): open HellForge.exe once to set up (or repair) the installation.");
        return (install, paths);
    }

    // An explicit --sandbox wins; otherwise the saved setting, where it can be offered. Linux must have the program
    // before the wrapper starts; without it the session is refused rather than falling through.
    static bool SandboxChoice(Dictionary<string, string> options, InstallSessionPaths paths)
    {
        var sandbox = options.ContainsKey("--sandbox") ? Yes(options, "--sandbox", false) : SessionRunner.SandboxByDefault(paths);
        if (sandbox && OperatingSystem.IsLinux() && SessionRunner.LinuxSandboxProgramProblem(paths) is { } sandboxProblem)
            throw new InvalidDataException("The Linux sandbox could not start: " + sandboxProblem +
                ". The game was not started. To run without the sandbox, turn off Settings > Sandbox.");
        return sandbox;
    }

    // play: the launcher's saved choices (launcher-settings.json), each replaced by its option when given.
    public static int Play(Dictionary<string, string> options)
    {
        var (install, paths) = Preflight(options);
        var war = options.GetValueOrDefault("--war") ?? ProfileStore.LoadWar(paths);
        if (war is not (ProfileStore.WarLive or ProfileStore.WarFrozen or ProfileStore.WarOpen))
            throw new ArgumentException($"--war takes {ProfileStore.WarLive}, {ProfileStore.WarFrozen} or {ProfileStore.WarOpen}.");
        var sandbox = SandboxChoice(options, paths);
        var session = new SessionOptions(Yes(options, "--local-playfab", ProfileStore.LoadLocalPlayFab(paths)),
            Yes(options, "--multiplayer", ProfileStore.LoadSdkShims(paths)), war,
            Yes(options, "--engine-log", ProfileStore.LoadEngineLog(paths)), Yes(options, "--crash-dump", ProfileStore.LoadCrashDump(paths)),
            GameLaunchOptions.Parse(options.TryGetValue("--game-options", out var typed) ? typed : ProfileStore.LoadLaunchOptions(paths)),
            Sandbox: sandbox, FullOwnershipCheck: Yes(options, "--full-ownership-check", false),
            SteamUdp: Yes(options, "--steam-udp", ProfileStore.LoadSteamUdp(paths)));
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
        var previousRunId = SessionRunner.LatestRunId(paths);
        StartRunner(runner, paths, profile, session);
        string? run;
        while ((run = SessionRunner.RunSince(paths, previousRunId)) == null)
        {
            if (!runner.IsRunning)
                throw new InvalidDataException(SessionRunner.LinuxContainerRefusal(install.Root)
                    ?? "The session runner exited before starting: " + RunnerLog(install, startedUtc));
            Thread.Sleep(250);
        }
        Emit(new { @event = "started", runId = Path.GetFileName(run), run, pid = runner.ProcessId, profile = profile.Name, war, arguments });
        return Yes(options, "--wait", false) ? Follow(run, () => runner.IsRunning) : 0;
    }

    // Starts the session runner; a start Windows refuses is said as the launcher says it, Smart App Control's blocks named
    static void StartRunner(SessionRunner runner, ISessionPaths paths, LocalProfile profile, SessionOptions session)
    {
        try { runner.Start(paths, profile, session); }
        catch (System.ComponentModel.Win32Exception error) when (HellForge.Core.SmartAppControl.IsBlock(error.NativeErrorCode))
        {
            throw new InvalidDataException(HellForge.Core.SmartAppControl.BlockedMessage(paths.Session, HellForge.Core.SmartAppControl.Current));
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or PlatformNotSupportedException)
        {
            throw new InvalidDataException("Could not start the session runner: " + error.Message);
        }
    }

    // Why the session runner stopped before it made its run folder: its reason in logs/session-runner.log, if new.
    static string RunnerLog(InstallLayout install, DateTime sinceUtc) =>
        EarlySessionFailure.Reason(install.Logs, sinceUtc) ?? "see " + Path.Combine(install.Logs, EarlySessionFailure.RunnerLog);

    // Reports the run's status (result.json) as it changes, until the session runner has exited. 0 when the session
    // ended normally.
    static int Follow(string run, Func<bool> running)
    {
        var result = Path.Combine(run, "result.json");
        var last = "";
        var problemsShown = false;
        string? steamInput = null;
        while (true)
        {
            var alive = running();
            if (SessionFiles.ReadJson(result) is { } state)
            {
                problemsShown = problemsShown || EmitProblems(state);
                steamInput = EmitSteamInput(state, steamInput);
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

    // Sandboxed: the controllers Steam Input handles cannot reach the game (SteamInputNote). The note when it changes;
    // returns the note reported last.
    static string? EmitSteamInput(JsonElement state, string? last)
    {
        if (SteamInputNote.Of(state) is not { } note || note == last) return last;
        Emit(new { @event = "steam-input", note });
        return note;
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
        // --now yes: the game is ended at once instead of being asked to close first (and save its shader cache)
        SessionRunner.SignalStop(run, Yes(options, "--now", false));
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
                steamUdp = ProfileStore.LoadSteamUdp(paths),
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

    // mods: the Mods page's list, top first (the bottom one loads first; gameplay mods, then cosmetic ones). A .rar or .7z
    // dropped into mods/ is added first (ModInstaller.TakeDropped), as the Mods page does.
    public static object Mods(Dictionary<string, string> options)
    {
        var install = Install(options);
        var dropped = ModInstaller.TakeDropped(install);
        dropped.AddRange(ModStaging.Notices(install)); // mod files written over through a staged link, as the Mods page shows them
        var (mods, problems) = ModCatalog.Load(install.Mods);
        problems.InsertRange(0, dropped);
        var active = ModCatalog.Active(mods);
        var gameplay = active.Count(ModCatalog.Counted);
        var sdk = mods.Any(m => m.HasSource) ? ModSdk.Find(install.Root) : null;
        var toolsReady = sdk != null && ModKit.ToolsReady(sdk);
        return new
        {
            mods = mods.Select(m => new
            {
                m.Id, m.Name, m.Version, m.Description, m.Enabled,
                // why this HellForge can't run it (the Mods page's UNSUPPORTED), or null
                unsupported = m.Unsupported,
                // a source mod's last build (ModBuilds.Status: state, when, the compiler's message), or null
                build = ModBuilds.Status(install, m, sdk, toolsReady),
                // the premium items it was built on (PremiumContent): the session locks it for an account that lacks them
                premium = m.Premium.Select(p => new { requirement = p, name = PremiumContent.Describe(p) }),
                m.Url, m.Icon,
                group = m.Cosmetic ? "cosmetic" : ModCatalog.Tier(m) == CoopTier.PerPlayer ? "per-player" : "gameplay",
                tags = Tags(m),
                m.Folder, archive = m.Archive != null ? Path.Combine(install.Mods, m.Archive) : null,
                removable = ModBatch.Removable(m),
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
        if (mod.HasNative) tags.Add("native");
        if (mod.Stats.Count > 0) tags.Add("stats");
        if (mod.DataFiles.Count > 0 || mod.HasDataSpec) tags.Add("data");
        if (mod.Pack != null) tags.Add("pack");
        if (mod.Lua) tags.Add("lua");
        if (BundledMods.ShippedFiles(mod.Folder) != null) tags.Add("bundled");
        if (mod.Id == ModCatalog.LocalStatsId) tags.Add("local-stats");
        if (mod.Premium.Count > 0) tags.Add("premium");
        if (mod.Unsupported != null) tags.Add("unsupported");
        return tags;
    }

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

    // mod-add with --file given more than once: every file is read before any is copied, and one that isn't a mod refuses
    // them all (ModBatch.Add).
    public static object ModAddMany(Dictionary<string, string> options, IReadOnlyList<string> files)
    {
        var install = Install(options);
        NoSession();
        var added = ModBatch.Add(install, files);
        return new
        {
            done = added.Count(a => a.Error == null),
            failed = added.Count(a => a.Error != null),
            mods = added.Select(a => new { a.Mod.Id, a.Mod.Name, a.Mod.Version, installed = a.Installed, replaced = a.Mod.Replaces, error = a.Error })
        };
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
        if (!ModBatch.Removable(mod)) throw new InvalidDataException($"{id} ships with HellForge: switch it off instead (mod-switch).");
        ModInstaller.Remove(install, mod);
        return new { mod.Id, removed = mod.Archive != null ? Path.Combine(install.Mods, mod.Archive) : mod.Folder };
    }

    // mod-remove --ids a,b,c: one read of the mods folder for the whole list (ModBatch.Remove).
    public static object ModRemoveMany(Dictionary<string, string> options, string ids)
    {
        var install = Install(options);
        NoSession();
        var removed = ModBatch.Remove(install, ModBatch.Ids(ids));
        return new
        {
            done = removed.Count(r => r.Error == null),
            failed = removed.Count(r => r.Error != null),
            mods = removed.Select(r => new { r.Id, removed = r.Path, error = r.Error })
        };
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

    // mod-switch --ids a,b,c --on yes | no: all of them, with one write, or none (ModBatch.Switch).
    public static object ModSwitchMany(Dictionary<string, string> options, string ids, string on)
    {
        var install = Install(options);
        NoSession();
        var enabled = on switch { "yes" => true, "no" => false, _ => throw new ArgumentException("--on takes yes or no.") };
        var switched = ModBatch.Switch(install, ModBatch.Ids(ids), enabled);
        return new { done = switched.Count, failed = 0, enabled, mods = switched.Select(id => new { id, enabled }) };
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

    // --strangers: yes/no are the player's choice; default removes it, so the sandbox decides.
    static bool? StrangersChoice(string value) => value switch
    {
        "yes" => true,
        "no" => false,
        "default" => null,
        _ => throw new ArgumentException("--strangers takes yes, no or default.")
    };

    // The settings' yes/no options, each saved when given; Bare is what the option alone (no value) means.
    static readonly (string Option, bool Bare, Action<ISessionPaths, bool> Save)[] Switches =
    [
        ("--local-playfab", true, ProfileStore.SaveLocalPlayFab),
        ("--multiplayer", true, ProfileStore.SaveSdkShims),
        ("--engine-log", true, ProfileStore.SaveEngineLog),
        ("--crash-dump", false, ProfileStore.SaveCrashDump),
        ("--sandbox", false, ProfileStore.SaveSandbox),
        ("--steam-udp", false, ProfileStore.SaveSteamUdp),
        ("--deal-subfactions", true, ProfileStore.SaveDealSubfactions),
        ("--deal-seaf", true, ProfileStore.SaveDealSeaf),
    ];

    // The mod sync switches, kept with the mods (hellforge-mods.json) as the Mods page's Sharing card keeps them.
    static readonly (string Option, Action<string, bool> Save)[] ModSyncSwitches =
    [
        ("--share-mods", ModSettings.SetShare),
        ("--list-public", ModSettings.SetListPublic),
        ("--show-edited-stats", ModSettings.SetShowEditedStats),
        ("--host-stats", ModSettings.SetHostStats),
    ];

    static void SaveSwitches(Dictionary<string, string> options, InstallSessionPaths paths)
    {
        foreach (var (option, bare, save) in Switches)
            if (options.ContainsKey(option)) save(paths, Yes(options, option, bare));
    }

    // The war seed (--war-seed): a whole number, or random, rolled as the launcher's button rolls it.
    static void SaveWarSeed(Dictionary<string, string> options, InstallSessionPaths paths)
    {
        if (!options.TryGetValue("--war-seed", out var seedText)) return;
        ProfileStore.SaveSubfactionSeed(paths, seedText == "random"
            ? (uint)System.Security.Cryptography.RandomNumberGenerator.GetInt32(1, int.MaxValue)
            : uint.TryParse(seedText, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var seed)
                ? seed : throw new ArgumentException("--war-seed takes a whole number, or random."));
    }

    // The settings kept with the mods (hellforge-mods.json): the mod sync switches, strangers, live changes, Forge Credits.
    static void SaveModSettings(Dictionary<string, string> options, string modsFolder)
    {
        foreach (var (option, save) in ModSyncSwitches)
            if (options.ContainsKey(option)) save(modsFolder, Yes(options, option, false));
        // experimental: the next session's mods may change while the game runs
        if (options.ContainsKey("--live-changes")) ModSettings.SetLiveChanges(modsFolder, Yes(options, "--live-changes", false));
        // Forge Credits in this share (0 to 100) of the loot caches at points of interest; default is HellForge's (30).
        if (options.TryGetValue("--forge-credits", out var forgeCredits))
            ModSettings.SetForgeCreditChance(modsFolder, forgeCredits is "default" or "off" ? null
                : int.TryParse(forgeCredits, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var percent) && percent <= 100
                    ? percent : throw new ArgumentException("--forge-credits takes a percentage from 0 to 100, or default."));
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
        SaveSwitches(options, paths);
        if (options.TryGetValue("--game-options", out var typed))
        {
            _ = GameLaunchOptions.Parse(typed); // refused as play refuses it: never a switch the session sets itself
            ProfileStore.SaveLaunchOptions(paths, typed);
        }
        // Play with strangers is the mods settings' (hellforge-mods.json), which the backend reads, even during a session.
        // yes/no stores the player's choice; default removes it, so a sandboxed session turns strangers on.
        if (options.TryGetValue("--strangers", out var strangers))
            ModSettings.SetPlayWithStrangers(install.Mods, StrangersChoice(strangers));
        SaveWarSeed(options, paths);
        SaveModSettings(options, install.Mods);
        var chosenStrangers = ModSettings.PlayWithStrangers(install.Mods);
        var modSettings = ModSettings.Read(install.Mods);
        var sandboxed = SessionRunner.SandboxByDefault(paths);
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
            steamUdp = ProfileStore.LoadSteamUdp(paths),
            steamBridge = SessionRunner.HasSteamBridge(paths),
            playWithStrangers = ModSettings.StrangersEffective(chosenStrangers, sandboxed),
            playWithStrangersChosen = chosenStrangers,
            liveChanges = ModSettings.LiveChanges(install.Mods),
            forgeCreditChance = ModSettings.ForgeCreditChance(install.Mods) ?? ModSettings.DefaultForgeCreditChance,
            dealSubfactions = ProfileStore.LoadDealSubfactions(paths),
            dealSeaf = ProfileStore.LoadDealSeaf(paths),
            warSeed = ProfileStore.LoadSubfactionSeed(paths),
            shareMods = modSettings.Share,
            listPublic = modSettings.ListPublic,
            showEditedStats = modSettings.ShowEditedStats,
            hostStats = modSettings.HostStats,
            file = paths.SettingsFile
        };
    }

    // mod-sync: what mod sync keeps for the player, as the Mods page's Sharing card lists it: the mods they trusted on the
    // in-game MOD SYNC prompt, the mods they blocked, and the mods downloaded from hosts. --untrust <id> --hash <hash> takes
    // a trust back and deletes that download; --block / --unblock <id> change the blocklist; --delete-downloaded yes
    // deletes every download. Untrusting and deleting wait for the session to end, as on the Mods page.
    public static object ModSync(Dictionary<string, string> options)
    {
        var install = Install(options);
        if (options.TryGetValue("--untrust", out var untrust))
        {
            NoSession();
            var hash = options.GetValueOrDefault("--hash") ?? throw new ArgumentException("--untrust needs the trusted mod's --hash (mod-sync lists it).");
            if (!ModSettings.Read(install.Mods).Trusted.Any(t => t.Id == untrust && t.Hash == hash))
                throw new ArgumentException($"{untrust} with that hash is not trusted.");
            ModSettings.Untrust(install.Mods, untrust, hash);
            SyncedStore.Forget(install.Mods, untrust, hash);
        }
        else if (options.ContainsKey("--hash")) throw new ArgumentException("--hash goes with --untrust.");
        if (options.TryGetValue("--block", out var block))
        {
            if (!ModCatalog.IsModId(block)) throw new ArgumentException($"{block} is not a mod id.");
            ModSettings.SetBlocked(install.Mods, block, true);
        }
        if (options.TryGetValue("--unblock", out var unblock)) ModSettings.SetBlocked(install.Mods, unblock, false);
        if (Yes(options, "--delete-downloaded", false))
        {
            NoSession();
            var store = SyncedStore.Root(install.Mods);
            if (Directory.Exists(store)) Directory.Delete(store, recursive: true);
        }
        var settings = ModSettings.Read(install.Mods);
        var (count, bytes) = SyncedStore.Downloaded(install.Mods);
        return new
        {
            trusted = settings.Trusted.Select(t => new { t.Id, t.Name, t.Version, t.Hash, t.From, t.Since, t.Stranger, t.Sandboxed, sandboxOnly = t.SandboxOnly }),
            blocked = settings.Blocked.Order(StringComparer.Ordinal),
            downloaded = new { count, bytes, folder = SyncedStore.Root(install.Mods) }
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
            var run = picked ?? runs.FirstOrDefault();
            target = Path.GetFullPath(target);
            if (File.Exists(target)) throw new ArgumentException($"{target} exists already.");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            // No session yet: the logs of a start that stopped before its session folder existed, when there are any.
            var exported = run != null ? SessionLogs.Export(run.Folder, Path.Combine(install.UserData, "logs"), target, install.Logs)
                : SessionLogs.ExportStart(install.Logs, target) ?? throw new InvalidDataException("No session logs yet: play a session first.");
            return new { run = run != null ? Describe(run) : null, exported.Zip, exported.Files, exported.Skipped };
        }
        var runnerLog = Path.Combine(install.Logs, "session-runner.log");
        return new
        {
            runs = runs.Select(Describe),
            files = picked == null ? null : LocalInstall.FilesNoLinks(picked.Folder)
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
        var order = listed.Concat(mods.Select(m => m.Id).Where(id => !listed.Contains(id))).ToList();
        ModSettings.SetOrder(install.Mods, order);
        // the mods read above, in the order just saved: the folder isn't read a second time
        return new { order = ModCatalog.Ordered(mods, order).Select(m => m.Id) };
    }
}
