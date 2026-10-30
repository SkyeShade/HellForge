using System.Text.Json;
using HellForge.Cli;
using HellForge.Core;
using HellForge.Core.GameData;

try
{
    if (args.Length == 0 || args[0] is "help" or "--help" or "-h")
    {
        Console.WriteLine("""
            HellForge - local installation research

            discover [--steam-root <Steam folder>]
            inspect --manifest <appmanifest_553850.acf>
            prepare --manifest <appmanifest_553850.acf> --destination <new folder>
                    --build <build ID> --exe-sha256 <SHA256> --game-sha256 <SHA256>
            supported-build --manifest <appmanifest_553850.acf> --depotcache <Steam/depotcache> --out <file>
            supported-language --name <german, japanese, ...> --dlc <app ID> --manifest <manifest ID>
                    --depot-folder <download_depot's depot_<id> folder> [--depotcache <Steam/depotcache>] --out <file>
            verify-game --game <game folder> [--cache <file>]
            install-game --manifest <appmanifest_553850.acf> --destination <game folder>
            create-install --root <folder>
            prepare-session --install <folder> --profile <name> [--user-data <root>] [--verify-all yes | no]
            migrate --install <folder> --from <artifacts/host-startup> [--appdata <folder>]
            mods-stage --install <folder> [--plan <mod loader plan file> --logs <session folder>] [--modset <file>] [--sync <mod sync session file>] [--sandbox yes|no]
            build-source-mods --install <folder> [--progress <file>]
            build-data --mod <mod folder> [--install <folder> | --game-data <game data folder> [--steam-data <Steam's game data folder>] --out <folder>]
            check-stats --mod <mod folder>
            sdk-headers --repo <HellForge source folder> [--mode write | check]
            new-mod --id <owner.mod-name> --name <name> (--install <folder> | --out <folder>)
            build-mod --mod <mod folder>
            package-mod --mod <mod folder> [--out <folder>]
            mod-hash --mod <mod folder or .zip> [--install <folder>]
            verified-list-keygen --out <private key file>
            verified-list-sign --list <verified-mods.json> --key <private key file>
            verified-list-check --list <verified-mods.json> [--signature <file>]
            mod-tools --install <installation or source folder> [--ide yes]
            steam-bridge host | inner | check [socket]

            For launchers of their own (docs/external-launchers.md); --install defaults to the installation this is in:
            play [--install <folder>] [--profile <name>] [--war live | frozen | open] [--local-playfab yes | no]
                 [--multiplayer yes | no] [--engine-log yes | no] [--crash-dump yes | no] [--sandbox yes | no]
                 [--steam-udp yes | no] [--game-options <options>] [--full-ownership-check yes | no] [--wait yes] [--dry-run yes]
            stop [--install <folder>] [--run <session folder>] [--wait yes]
            status [--install <folder>]
            profiles [--install <folder>] [--create <name> --template <template>]
            fetch-war [--install <folder>]
            mods [--install <folder>]
            languages [--install <folder>] [--steam-root <Steam folder>]
            language-add [--install <folder>] --name <language> [--steam-root <Steam folder>] [--keep yes | no]
            mod-add [--install <folder>] --file <.zip, .rar, .7z, hellforge-mod.json or manifest.json>
            mod-remove [--install <folder>] --id <mod id>
            mod-switch [--install <folder>] --id <mod id> --on yes | no
            mod-order [--install <folder>] --ids <id,id,... top first>
            mod-pack [--install <folder>] --id <pack id> (--option <name> [--on yes | no] [--variant <name>] | --all yes | no)
            settings [--install <folder>] [--profile <name>] [--war live | frozen | open] [--local-playfab yes | no]
                     [--multiplayer yes | no] [--engine-log yes | no] [--crash-dump yes | no] [--sandbox yes | no]
                     [--steam-udp yes | no] [--strangers yes | no] [--game-options <options>]
            logs [--install <folder>] [--run <session id>] [--export <file.zip> | --crash-report <file.zip> [--dump yes | no]]

            inspect prints JSON; prepare makes a full, hash-verified, unpatched copy; supported-build lists a
            Steam-validated install's depot files and hashes (names and hashes only) for HellForge to target.
            supported-language adds a language pack, downloaded by Steam at the build's manifest, to a language list
            (src/HellForge.Core/supported-languages.json). languages lists the pinned packs (in the copy, installed in
            Steam, the download_depot command for each); language-add copies one from Steam's download into the copy.
            build-data builds a mod's hellforge-data.json from the game data: into the installation's cache (as a
            session does), or into --out; with neither it only checks the spec.
            sdk-headers generates the mod SDK's typed headers and the loader's field table from mappings/; check
            only lists the files that differ (exit code 1 if any).
            new-mod makes a code mod from the SDK's template: in the installation's mods folder (built there, it is
            installed) or in --out. build-mod compiles a code mod with the SDK's pinned Zig: in an installation's mods
            folder into the build a session loads (temp/mod-builds; sessions build source mods themselves), elsewhere into
            the DLL its manifest names. build-source-mods builds every enabled source mod into temp/mod-builds before a
            session: a Linux launcher runs it natively with the pinned Linux Zig, so the session never compiles under Wine.
            package-mod compiles a source mod, then zips it to share (the installation's exports
            folder, or --out): sources and files, no DLL a player's HellForge would rebuild, no IDE leftovers.
            mod-hash prints a mod's content hash as mod sync and HellForge's verified list (data/verified-mods.json) take
            it: a folder by its files, a zip by what it ships, as a player's HellForge loads it; with its class and an entry
            for the verified list.
            mod-tools downloads that Zig (and CMake and Ninja for IDEs with --ide yes), pinned and checksummed.
            steam-bridge is the Linux wrapper's relay to Steam instead of socat: host exposes Steam's loopback ports as
            hf-steam-*.sock files, inner recreates them inside the offline network namespace, and check exits 0 when a
            bridge or the named socket answers.
            play starts a session as HellForge.exe's PLAY does, with the launcher's saved choices unless an option
            replaces one; the engine's session runner does every check and cleanup itself. Without --wait it prints the
            run once started and returns; with it, one JSON line per status until the session ends (exit code 0 when it
            ended normally). stop asks the session to close the game. Mod changes wait until no session runs.
            settings saves the choices play and the launcher start from, and prints them. logs lists the sessions,
            newest first (--run: one session's files); --export zips one as the launcher's Save logs to Desktop does;
            --crash-report makes the crash report the launcher's Save crash report makes (scrubbed further, for a public
            issue), with the crash dump only with --dump yes. Nothing is uploaded.
            sandbox-readiness --install <folder>
            sandbox-prefix-case --install <folder>
            sandbox-supervise --install <folder> --prefix <compatdata> --session-file <play-session.json> --parent-pid <pid>
            sandbox-copy-back --install <folder> --run <session folder> --profile <name>
            The sandbox commands are native Linux only: sandbox-readiness runs the wrapper's self-test and records the
            result for the installed app build and kernel (first making HellForge's prefix case-sensitive, as
            sandbox-prefix-case does alone, when Proton made its drive_c case-insensitive); hf-inner.sh starts the supervisor, which builds the game
            domain and performs the hardened copy-back after the domain is gone. The supervisor and copy-back never
            invoke a shell or start an unsandboxed game.
            Close Steam and the game before prepare; installations must remain idle.
            Failed preparations retain preparing.lock and must not be used.
            """);
        return 0;
    }
    // Before the --option parser: host and inner take a subcommand, not options.
    if (args[0] == "steam-bridge") return SteamBridgeCommand.Run(args);
    if (SandboxCommands.TryRun(args, out var sandboxExit)) return sandboxExit;
    var allowed = args[0] switch
    {
        "discover" => new[] { "--steam-root" },
        "inspect" => ["--manifest"],
        "prepare" => ["--manifest", "--destination", "--build", "--exe-sha256", "--game-sha256"],
        "supported-build" => ["--manifest", "--depotcache", "--out"],
        "supported-language" => ["--name", "--dlc", "--manifest", "--depot-folder", "--depotcache", "--out"],
        "languages" => ["--install", "--steam-root"],
        "language-add" => ["--install", "--name", "--steam-root", "--keep"],
        "verify-game" => ["--game", "--cache"],
        "crash-dump" => ["--pid", "--thread", "--pointers", "--out"],
        "install-game" => ["--manifest", "--destination"],
        "create-install" => ["--root"],
        "prepare-session" => ["--install", "--profile", "--user-data", "--verify-all"],
        "migrate" => ["--install", "--from", "--appdata"],
        "mods-stage" => ["--install", "--plan", "--logs", "--progress", "--modset", "--sync", "--sandbox", "--premium-snapshot", "--premium-locked"],
        "build-source-mods" => ["--install", "--progress"],
        "build-data" => ["--mod", "--install", "--game-data", "--steam-data", "--out"],
        "check-stats" => ["--mod"],
        "sdk-headers" => ["--repo", "--mode"],
        "editor-data" => ["--out", "--install", "--game"],
        "new-mod" => ["--id", "--name", "--install", "--out"],
        "build-mod" => ["--mod"],
        "package-mod" => ["--mod", "--out"],
        "mod-hash" => ["--mod", "--install"],
        "verified-list-keygen" => ["--out"],
        "verified-list-sign" => ["--list", "--key"],
        "verified-list-check" => ["--list", "--signature"],
        "mod-tools" => ["--install", "--ide"],
        "play" => ["--install", "--profile", "--war", "--local-playfab", "--multiplayer", "--engine-log", "--crash-dump", "--sandbox", "--steam-udp", "--game-options", "--full-ownership-check", "--wait", "--dry-run"],
        "stop" => ["--install", "--run", "--wait"],
        "status" or "fetch-war" or "mods" => ["--install"],
        "profiles" => ["--install", "--create", "--template"],
        "mod-add" => ["--install", "--file"],
        "mod-remove" => ["--install", "--id"],
        "mod-switch" => ["--install", "--id", "--on"],
        "mod-order" => ["--install", "--ids"],
        "mod-pack" => ["--install", "--id", "--option", "--on", "--variant", "--all"],
        "settings" => ["--install", "--profile", "--war", "--local-playfab", "--multiplayer", "--engine-log", "--crash-dump", "--sandbox", "--steam-udp", "--strangers", "--game-options"],
        "logs" => ["--install", "--run", "--export", "--crash-report", "--dump"],
        _ => throw new ArgumentException($"Unknown command: {args[0]}")
    };
    var options = new Dictionary<string, string>(StringComparer.Ordinal);
    for (var i = 1; i < args.Length; i += 2)
    {
        if (!allowed.Contains(args[i]) || i + 1 >= args.Length || !options.TryAdd(args[i], args[i + 1]))
            throw new ArgumentException($"Unknown, duplicate, or incomplete option: {args[i]}");
    }
    string Required(string key) => options.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value)
        ? value : throw new ArgumentException($"Missing {key}.");
    string Sha(string key)
    {
        var value = Required(key);
        if (value.Length != 64 || !value.All(Uri.IsHexDigit)) throw new ArgumentException($"{key} must contain 64 hexadecimal digits.");
        return value.ToUpperInvariant();
    }
    object result;
    // --progress: a JSON file the launcher reads for its "compiling mods" bar (written whole, then moved in place).
    void WriteProgress(string? file, ModBuildProgress p)
    {
        if (file == null) return;
        try
        {
            File.WriteAllText(file + ".partial", JsonSerializer.Serialize(new { stage = p.Stage, done = p.Done, total = p.Total, mod = p.Mod }));
            File.Move(file + ".partial", file, overwrite: true);
        }
        catch (IOException) { } /* the launcher is reading it: the next one */
    }
    switch (args[0])
    {
        case "play":
            return Launch.Play(options);
        case "stop":
            return Launch.Stop(options);
        case "status":
            result = Launch.Status(options);
            break;
        case "profiles":
            result = Launch.Profiles(options);
            break;
        case "fetch-war":
            result = Launch.FetchWar(options);
            break;
        case "mods":
            result = Launch.Mods(options);
            break;
        case "mod-add":
            result = Launch.ModAdd(options, Required("--file"));
            break;
        case "languages":
            result = Launch.Languages(options);
            break;
        case "language-add":
            result = Launch.LanguageAdd(options, Required("--name"));
            break;
        case "mod-remove":
            result = Launch.ModRemove(options, Required("--id"));
            break;
        case "mod-switch":
            result = Launch.ModSwitch(options, Required("--id"), Required("--on"));
            break;
        case "mod-order":
            result = Launch.ModOrder(options, Required("--ids"));
            break;
        case "mod-pack":
            result = Launch.ModPack(options, Required("--id"));
            break;
        case "settings":
            result = Launch.Settings(options);
            break;
        case "logs":
            result = Launch.Logs(options);
            break;
        case "discover":
            var steam = options.GetValueOrDefault("--steam-root") ??
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
            result = LocalInstall.FindManifests(steam);
            break;
        case "inspect":
            result = LocalInstall.Inspect(Required("--manifest"));
            break;
        case "supported-build":
            var build = SupportedBuild.FromSteam(Required("--manifest"), Required("--depotcache"));
            build.Save(Required("--out"));
            result = new { build.BuildId, depots = build.Depots.Count, files = build.Files.Count, bytes = build.TotalBytes };
            break;
        case "supported-language":
            var language = SupportedLanguages.FromDownload(Required("--name"),
                uint.TryParse(Required("--dlc"), out var dlc) ? dlc : throw new ArgumentException("--dlc is the language DLC's app ID."),
                Required("--manifest"), Required("--depot-folder"), options.GetValueOrDefault("--depotcache"));
            SupportedLanguages.With(Required("--out"), language).Save(Required("--out"));
            result = new { language.Name, language.DlcAppId, language.ManifestId, files = language.Files.Count, bytes = language.TotalBytes };
            break;
        case "install-game":
            var target = SupportedBuild.Current;
            var installed = LocalInstall.Inspect(Required("--manifest"));
            if (installed.Pin.BuildId != target.BuildId)
                throw new InvalidDataException($"Steam has HD2 build {installed.Pin.BuildId}; HellForge supports build {target.BuildId}.");
            var reported = -1;
            var clock = System.Diagnostics.Stopwatch.StartNew();
            GameCopier.Copy(installed.Source, Required("--destination"), target, (done, total, _) =>
            {
                var percent = (int)(done * 100 / total);
                if (percent / 5 == reported / 5) return;
                reported = percent;
                Console.Error.WriteLine($"Copied {percent}% ({done >> 20} of {total >> 20} MB, {(int)clock.Elapsed.TotalSeconds} s)");
            });
            result = new { target.BuildId, files = target.Files.Count, bytes = target.TotalBytes, destination = Required("--destination"),
                seconds = (int)clock.Elapsed.TotalSeconds };
            break;
        case "create-install":
            result = new InstallLayout(Required("--root")).Create(SupportedBuild.Current.BuildId);
            break;
        case "prepare-session":
            var install = new InstallLayout(Required("--install"));
            if (!install.Exists) throw new InvalidDataException($"{install.Root} is not a HellForge installation.");
            var vanilla = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "Helldivers2");
            var verifyAll = options.GetValueOrDefault("--verify-all") switch
            {
                null or "no" => false,
                "yes" => true,
                _ => throw new ArgumentException("--verify-all is yes or no.")
            };
            var prepared = GameSettings.PrepareSession(install, Required("--profile"), vanilla, options.GetValueOrDefault("--user-data"), verifyAll);
            Console.WriteLine(JsonSerializer.Serialize(prepared, CopyPreparer.JsonOptions));
            return prepared.Problems.Count == 0 ? 0 : 2;
        case "mods-stage":
        {
            // The installation's mods: data mods are staged into the copy, and the plan and modlist digest are printed.
            var modInstall = new InstallLayout(Required("--install"));
            if (!modInstall.Exists) throw new InvalidDataException($"{modInstall.Root} is not a HellForge installation.");
            // the published verified list this session's runner fetched (VerifiedMods), for the synced mods' checks
            VerifiedMods.UseInstallation(modInstall);
            // The mods HellForge ships (the in-game editor) are put in place, or updated, first.
            var bundledProblems = BundledMods.Sync(modInstall, Console.Error.WriteLine);
            // a .rar or .7z dropped into mods/ is added as a folder first (ModInstaller.TakeDropped)
            bundledProblems.AddRange(ModInstaller.TakeDropped(modInstall));
            var (mods, modProblems) = ModCatalog.Load(Path.Combine(modInstall.Root, "mods"));
            modProblems.InsertRange(0, bundledProblems);
            var loadOrder = ModCatalog.LoadOrder(ModCatalog.Active(mods)); // bottom of the Mods page first
            // --sync: the session after a mod sync plays the host's gameplay mods (ModSyncSession); if they are not all here
            // as the host has them, the player's own mods, and the problem is reported. --sandbox yes: the session's game is
            // sandboxed, which a mod trusted from a stranger needs.
            string? syncedWith = null;
            var stageSandboxed = options.GetValueOrDefault("--sandbox") is { } sandboxFlag
                ? sandboxFlag == "yes" ? true : sandboxFlag == "no" ? false : throw new ArgumentException("--sandbox is yes or no.") : false;
            if (options.TryGetValue("--sync", out var syncFile))
            {
                try
                {
                    var sync = ModSyncSession.Read(syncFile);
                    var (synced, syncProblem) = sync.Resolve(modInstall.Mods, mods, sandboxed: stageSandboxed);
                    if (synced != null) (loadOrder, syncedWith) = (synced, sync.HostName.Length > 0 ? sync.HostName : sync.Host);
                    else modProblems.Add("Mod sync: playing your own mods, because " + syncProblem);
                    foreach (var mod in loadOrder.Where(m => m.Folder.StartsWith(SyncedStore.Root(modInstall.Mods), StringComparison.OrdinalIgnoreCase)))
                        SyncedStore.Touch(mod.Folder);
                }
                catch (Exception error) when (error is InvalidDataException or IOException or System.Text.Json.JsonException)
                {
                    modProblems.Add("Mod sync: playing your own mods, because the sync file could not be read (" + error.Message + ").");
                }
            }
            // The synced store keeps the mods used most recently (SyncedStore.Keep), this session's among them.
            foreach (var pruned in SyncedStore.Prune(modInstall.Mods, loadOrder.Select(m => m.Folder)))
                Console.Error.WriteLine("Mod sync: removed the downloaded mod " + pruned + ", used least recently.");
            // Premium content mods declare (PremiumContent), against the profile's account snapshot (--premium-snapshot): a
            // cosmetic mod the player does not own is not loaded; a gameplay one loads, its items locked by the backend
            // (--premium-locked, which the session passes on).
            var premium = PremiumMods.ForSession(modInstall, options.GetValueOrDefault("--premium-snapshot"), SteamAccount.Current(), loadOrder);
            loadOrder = premium.Loaded;
            modProblems.AddRange(premium.Problems);
            if (options.TryGetValue("--premium-locked", out var lockedFile)) PremiumMods.WriteLocked(lockedFile, premium.Locked);
            // Mods with a data spec get their data built from the copy's game data first (once per game data and spec);
            // a mod whose data cannot be built is not loaded.
            var (active, dataProblems) = ModDataCache.Prepare(modInstall, loadOrder, Console.Error.WriteLine);
            modProblems.AddRange(dataProblems);
            // Source mods are built (or their cached build reused) for this session; one that does not compile stays off.
            var progressFile = options.GetValueOrDefault("--progress");
            var (built, buildProblems) = ModBuilds.Prepare(modInstall, ModSdk.Find(modInstall.Root), active, SupportedBuild.Current.BuildId, Console.Error.WriteLine,
                progress: p => WriteProgress(progressFile, p));
            active = built;
            modProblems.AddRange(buildProblems);
            var staged = ModStaging.Stage(modInstall, active);
            // Code, stats and legacy component mods all need the in-process loader plan.
            var code = active.Where(m => m.NeedsLoader).ToList();
            if (options.TryGetValue("--plan", out var planPath))
                File.WriteAllText(planPath, ModLoaderPlan.Text(code, Path.GetFullPath(Required("--logs")), EntityProfile.Current, SupportedBuild.Current.BuildId));
            // The gameplay mods with their content hashes, for the backend (mod sync: joiners whose mods differ get it), and
            // the digest's mods one by one (ModlistDiff's wire form), so a refused co-op join can say which differ.
            var modSet = ModSet.Of(active);
            if (options.TryGetValue("--modset", out var modSetPath)) modSet.Write(modSetPath);
            var (digest, compared) = (modSet.Digest, modSet.Fingerprints());
            result = new
            {
                mods = active.Select(m => new { m.Id, m.Name, m.Version, m.Gameplay, native = m.HasNative, code = m.CodeDll, source = m.HasSource, dataFiles = m.DataFiles.Count, dataSpec = m.HasDataSpec }),
                codeMods = code.Count,
                loaderRequired = true, // read-only stats snapshots are also collected without mods
                disabled = mods.Where(m => !m.Enabled).Select(m => m.Id),
                problems = modProblems, staged, stagedCopied = ModStaging.LastCounts.Copied, stagedKept = ModStaging.LastCounts.Kept,
                native = active.Any(m => m.HasNative), digest, modlist = ModlistDiff.Encode(compared), syncedWith, premiumLocked = premium.Locked,
                // the session's mods with network type edits (after a mod sync, the host's), for the session's native applier
                nativeFolders = active.Where(m => m.NetworkFieldTypes.Count > 0).Select(m => Path.GetFullPath(m.Folder))
            };
            break;
        }
        case "build-source-mods":
        {
            // The Linux wrapper's native agent: every enabled source mod built into temp/mod-builds before the session,
            // with the same key and record the session checks. A mod that fails is reported, and the session leaves it off.
            var buildInstall = new InstallLayout(Path.GetFullPath(Required("--install")));
            if (!buildInstall.Exists) throw new InvalidDataException($"{buildInstall.Root} is not a HellForge installation.");
            var (mods, loadProblems) = ModCatalog.Load(buildInstall.Mods);
            foreach (var problem in loadProblems) Console.Error.WriteLine(problem);
            var activeMods = ModCatalog.Active(mods);
            var sourceMods = activeMods.Count(m => m.HasSource);
            var progressFile = options.GetValueOrDefault("--progress");
            var (prebuilt, buildProblems) = ModBuilds.Prepare(buildInstall, ModSdk.Find(buildInstall.Root), activeMods, SupportedBuild.Current.BuildId,
                Console.Error.WriteLine, installTools: false, progress: p => WriteProgress(progressFile, p));
            result = new { install = buildInstall.Root, sourceMods, built = prebuilt.Count(m => m.HasSource), problems = buildProblems };
            break;
        }
        case "check-stats":
        {
            var statsMod = ModCatalog.Parse(Path.GetFullPath(Required("--mod")), EntityProfile.Current);
            if (statsMod.Stats.Count == 0) throw new InvalidDataException($"The mod has no {StatsData.FileName} edits.");
            result = new { statsMod.Id, edits = statsMod.Stats.Count, shared = statsMod.Stats.Count(e => e.Shared && !e.Own),
                unverified = statsMod.Stats.Count(e => e.Unverified), build = StatsSchema.Current.Build };
            break;
        }
        case "new-mod":
        {
            var inInstall = options.TryGetValue("--install", out var newModInstall);
            if (inInstall == options.ContainsKey("--out")) throw new ArgumentException("Pass --install <installation> or --out <folder>.");
            var place = Path.GetFullPath(inInstall ? newModInstall! : Required("--out"));
            var sdk = ModSdk.Find(place) ?? ModSdk.Find(Directory.GetCurrentDirectory()) ?? throw new IOException("The mod SDK was not found.");
            var parent = inInstall ? new InstallLayout(place) is { Exists: true } layout ? layout.Mods : throw new InvalidDataException($"{place} is not a HellForge installation.") : place;
            var created = ModKit.Create(sdk, parent, Required("--id"), Required("--name"), inInstall ? "" : "$env{USERPROFILE}/Documents/HellForge");
            result = new { folder = created, tools = ModKit.ToolsReady(sdk) ? "ready" : "missing: run mod-tools" };
            break;
        }
        case "build-mod":
        {
            var modFolder = Path.GetFullPath(Required("--mod"));
            // A mod in an installation's mods folder: the build a session makes and loads (its cache); elsewhere, beside it.
            var parentFolder = Path.GetDirectoryName(modFolder);
            var installRoot = parentFolder != null && Path.GetFileName(parentFolder) == "mods" ? Path.GetDirectoryName(parentFolder) : null;
            var layout = installRoot != null && new InstallLayout(installRoot).Exists ? new InstallLayout(installRoot) : null;
            var built = ModCatalog.Parse(modFolder, EntityProfile.Current); // the manifest, the stats file and the data spec, as a session checks them
            string dll;
            if (layout != null && built.HasSource)
            {
                // as a session does: the pinned Zig fetched on first need, then the session's build
                var (ready, buildProblems) = ModBuilds.Prepare(layout, ModSdk.Find(modFolder), [built], SupportedBuild.Current.BuildId, Console.Error.WriteLine);
                if (buildProblems.Count > 0) throw new InvalidDataException(string.Join("\n", buildProblems));
                dll = ready[0].BuiltDll!;
            }
            else dll = ModKit.Build(modFolder, output: Console.Error.WriteLine);
            result = new { built.Id, dll, kilobytes = new FileInfo(dll).Length / 1024, stats = built.Stats.Count, cached = layout != null && built.HasSource };
            break;
        }
        case "package-mod":
        {
            // A zip to share (ModPackage): a source mod is compiled first, so a mod that does not build is never packed.
            var modFolder = Path.GetFullPath(Required("--mod"));
            var parentFolder = Path.GetDirectoryName(modFolder);
            var installRoot = parentFolder != null && Path.GetFileName(parentFolder) == "mods" ? Path.GetDirectoryName(parentFolder) : null;
            var layout = installRoot != null && new InstallLayout(installRoot).Exists ? new InstallLayout(installRoot) : null;
            var mod = ModCatalog.Parse(modFolder, EntityProfile.Current);
            if (mod.HasSource)
            {
                if (layout != null)
                {
                    var (_, buildProblems) = ModBuilds.Prepare(layout, ModSdk.Find(modFolder), [mod], SupportedBuild.Current.BuildId, Console.Error.WriteLine);
                    if (buildProblems.Count > 0) throw new InvalidDataException(string.Join("\n", buildProblems));
                }
                else ModKit.Build(modFolder, into: Path.Combine(Path.GetTempPath(), $"hellforge-package-{Guid.NewGuid():N}.dll"), output: Console.Error.WriteLine);
            }
            var outFolder = options.TryGetValue("--out", out var packageOut) ? packageOut
                : layout?.Exports ?? throw new ArgumentException("Pass --out <folder>: the mod is not in an installation's mods folder.");
            var zipPath = ModPackage.Create(mod, outFolder);
            result = new { mod.Id, mod.Version, zip = zipPath, files = ModPackage.Files(mod).Count, kilobytes = new FileInfo(zipPath).Length / 1024 };
            break;
        }
        case "mod-hash":
        {
            var hashed = ModHash.Of(Path.GetFullPath(Required("--mod")));
            // --install: against that installation's saved published list (VerifiedMods), not only the embedded one
            if (options.TryGetValue("--install", out var hashInstall)) VerifiedMods.UseInstallation(new InstallLayout(hashInstall));
            result = new
            {
                hashed.Id, hashed.Name, hashed.Version, hashed.Hash, @class = ModSets.ClassName(hashed.Class), hashed.Files, hashed.Url,
                verified = VerifiedMods.Contains(hashed.Id, hashed.Hash), verifiedSerial = VerifiedMods.Current.Serial,
                entry = new { id = hashed.Id, name = hashed.Name, version = hashed.Version, hash = hashed.Hash, reviewed = "YYYY-MM-DD, reviewer: what was reviewed" }
            };
            break;
        }
        // The published verified list (docs/verified-list.md), for maintainers: a signing key pair, a list's signature, and
        // a check of a list and signature against the key this HellForge carries (VerifiedMods.PublicKey).
        case "verified-list-keygen":
        {
            var keyFile = Path.GetFullPath(Required("--out"));
            if (File.Exists(keyFile)) throw new IOException($"{keyFile} exists already; a key is never overwritten.");
            var (privateKey, publicKey) = VerifiedMods.NewKey();
            File.WriteAllText(keyFile, privateKey);
            result = new { privateKey = keyFile, publicKey, next = "Put publicKey in VerifiedMods.PublicKey; keep the private key off the server and out of the repository." };
            break;
        }
        case "verified-list-sign":
        {
            var listFile = Path.GetFullPath(Required("--list"));
            var list = File.ReadAllBytes(listFile);
            var pem = File.ReadAllText(Path.GetFullPath(Required("--key")));
            var signature = VerifiedMods.Sign(list, pem);
            var publicKey = VerifiedMods.PublicKeyOf(pem);
            var listing = VerifiedMods.Checked(list, signature, publicKey);
            File.WriteAllText(listFile + VerifiedMods.SignatureSuffix, signature);
            result = new
            {
                listing.Serial, mods = listing.Mods.Count, signature = listFile + VerifiedMods.SignatureSuffix, publicKey,
                hellForgeKey = publicKey == VerifiedMods.PublicKey, embeddedSerial = VerifiedMods.Embedded.Serial
            };
            break;
        }
        case "verified-list-check":
        {
            var listFile = Path.GetFullPath(Required("--list"));
            if (VerifiedMods.PublicKey.Length == 0) throw new InvalidDataException("This HellForge carries no key for the published list (VerifiedMods.PublicKey).");
            var listing = VerifiedMods.Checked(File.ReadAllBytes(listFile),
                File.ReadAllText(Path.GetFullPath(options.GetValueOrDefault("--signature") ?? listFile + VerifiedMods.SignatureSuffix)), VerifiedMods.PublicKey);
            result = new { listing.Serial, mods = listing.Mods.Select(m => $"{m.Id} {m.Version}"), embeddedSerial = VerifiedMods.Embedded.Serial };
            break;
        }
        case "mod-tools":
        {
            var sdk = ModSdk.Find(Required("--install")) ?? throw new IOException("The mod SDK was not found there.");
            var ide = options.GetValueOrDefault("--ide") is { } flag ? flag == "yes" ? true : flag == "no" ? false : throw new ArgumentException("--ide is yes or no.") : false;
            ModKit.InstallToolsAsync(sdk, ide, Console.Error.WriteLine).GetAwaiter().GetResult();
            result = new { tools = sdk.Tools, zig = OperatingSystem.IsLinux() ? sdk.NativeZig : sdk.Zig, ide };
            break;
        }
        case "editor-data":
        {
            // The in-game editor's catalogue, and its icons and fonts from the game data of --install (its game copy) or --game.
            var outFolder = Path.GetFullPath(Required("--out"));
            Directory.CreateDirectory(outFolder);
            File.WriteAllText(Path.Combine(outFolder, EditorCatalog.FileName), EditorCatalog.Text());
            var gameData = options.TryGetValue("--game", out var game) ? Path.GetFullPath(game)
                : options.TryGetValue("--install", out var editorInstall) ? Path.Combine(new InstallLayout(Path.GetFullPath(editorInstall)).Game, "data") : null;
            object icons = "skipped (no --install or --game)";
            if (gameData != null)
            {
                var (count, missing) = HellForge.Core.GameData.EditorIcons.Write(gameData, Path.Combine(outFolder, HellForge.Core.GameData.EditorIcons.FileName));
                icons = new { count, missing, fonts = HellForge.Core.GameData.EditorFonts.Write(gameData, outFolder) };
            }
            result = new { folder = outFolder, catalog = EditorCatalog.FileName, icons };
            break;
        }
        case "sdk-headers":
        {
            var mode = options.GetValueOrDefault("--mode") ?? "write";
            if (mode is not ("write" or "check")) throw new ArgumentException("--mode must be write or check.");
            var stale = SdkMappings.Sync(Path.GetFullPath(Required("--repo")), mode == "check");
            Console.WriteLine(JsonSerializer.Serialize(new { mode, stale }, CopyPreparer.JsonOptions));
            return mode == "check" && stale.Count > 0 ? 1 : 0;
        }
        case "build-data":
        {
            // A mod's data spec: checked, then built into the installation's cache (what the next session stages) or --out.
            var modFolder = Path.GetFullPath(Required("--mod"));
            var spec = DataSpecs.Load(modFolder) ?? throw new InvalidDataException($"{modFolder} has no {DataSpecs.FileName}.");
            var resources = spec.Patches.Sum(p => p.Resources.Sum(r => r is IncludeResources include ? include.Ids.Count : 1));
            if (options.TryGetValue("--install", out var installRoot))
            {
                if (options.ContainsKey("--game-data") || options.ContainsKey("--out")) throw new ArgumentException("--install builds into the installation's cache; use --game-data with --out instead.");
                var dataInstall = new InstallLayout(installRoot);
                if (!dataInstall.Exists) throw new InvalidDataException($"{dataInstall.Root} is not a HellForge installation.");
                var mod = ModCatalog.Parse(modFolder, EntityProfile.Current);
                GameDataReader? game = null;
                try
                {
                    var (folder, files, built) = ModDataCache.Ensure(dataInstall, mod, () => game = new GameDataReader(Path.Combine(dataInstall.Game, "data")), Console.Error.WriteLine);
                    result = new { mod = mod.Id, built, folder, files };
                }
                finally { game?.Dispose(); }
            }
            else if (options.ContainsKey("--game-data") || options.ContainsKey("--out"))
            {
                using var game = new GameDataReader(Required("--game-data"));
                var output = Path.GetFullPath(Required("--out"));
                if ((output + Path.DirectorySeparatorChar).StartsWith(game.Folder + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                    throw new ArgumentException("--out must not be inside the game data folder.");
                using var steamGame = options.TryGetValue("--steam-data", out var steamData) ? new GameDataReader(steamData) : null;
                result = new { output, files = ModDataBuilder.Build(spec, modFolder, game, output, steamGame) };
            }
            else result = new { checkedSpec = Path.Combine(modFolder, DataSpecs.FileName), patches = spec.Patches.Count, resources, inputs = spec.Inputs };
            break;
        }
        case "migrate":
            var destination = new InstallLayout(Required("--install"));
            if (!destination.Exists) throw new InvalidDataException($"{destination.Root} is not a HellForge installation.");
            result = Migration.Run(destination, Required("--from"), options.GetValueOrDefault("--appdata") ??
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "HD2BeyondTheStars"));
            break;
        case "crash-dump":
            // A crashed game's memory dump, from outside it (CrashDump.cs; started by the mod loader's crash note).
            result = CrashDump.Write(int.Parse(Required("--pid"), System.Globalization.CultureInfo.InvariantCulture),
                uint.Parse(Required("--thread"), System.Globalization.CultureInfo.InvariantCulture),
                ulong.Parse(Required("--pointers"), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture),
                Path.GetFullPath(Required("--out")));
            break;
        case "verify-game":
            var supported = SupportedBuild.Current;
            var lastPercent = -1;
            var problems = supported.Verify(Required("--game"), options.GetValueOrDefault("--cache"), (done, total) =>
            {
                var percent = (int)(done * 100 / total);
                if (percent / 10 != lastPercent / 10) Console.Error.WriteLine($"Verified {percent}%");
                lastPercent = percent;
            });
            result = new { supported.BuildId, files = supported.Files.Count, matches = problems.Count == 0, problems };
            if (problems.Count > 0)
            {
                Console.WriteLine(JsonSerializer.Serialize(result, CopyPreparer.JsonOptions));
                return 2;
            }
            break;
        default:
            result = CopyPreparer.Prepare(Required("--manifest"), Required("--destination"),
                new(Required("--build"), Sha("--exe-sha256"), Sha("--game-sha256")),
                (done, total) => Console.Error.WriteLine($"Verified {done}/{total} files"));
            break;
    }
    Console.WriteLine(JsonSerializer.Serialize(result, CopyPreparer.JsonOptions));
    return 0;
}
catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or System.Security.SecurityException)
{
    Console.Error.WriteLine($"HellForge: {error.Message}");
    return 1;
}
