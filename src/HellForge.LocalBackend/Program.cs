using System.Text.Json;
using HellForge.LocalBackend;

if (args.Contains("--help"))
{
    Console.WriteLine("HellForge.LocalBackend [--port 0..65535] [--mode local-session|reject-gmi] [--startup-fixtures true|false] [--profile-dir <directory>] [--lifetime-seconds 0..300] [--evidence <new directory>]");
    Console.WriteLine("HellForge.LocalBackend profile show <directory>");
    Console.WriteLine("HellForge.LocalBackend profile apply-template <directory> <template>   (replaces the inventory; server must be stopped)");
    Console.WriteLine("HellForge.LocalBackend profile add-template-items <directory> <template>   (adds only missing items; server must be stopped)");
    Console.WriteLine("HellForge.LocalBackend profile import-preview <directory> [template]   (the account snapshot against the profile, as JSON)");
    Console.WriteLine("HellForge.LocalBackend profile import <directory> [template]   (applies the account snapshot with the profile's account-import.json; backs up first; server must be stopped)");
    Console.WriteLine("HellForge.LocalBackend profile restore <directory> <backup file>   (backs up first; server must be stopped)");
    Console.WriteLine("--item-catalog <file> (development) serves that file's \"items\" array as /Progression/items.");
    Console.WriteLine("--profile-template <name> sets the inventory of a newly created profile: " + string.Join(", ", ProfileStore.TemplateNames));
    Console.WriteLine("Synthetic loopback-only login service. Default port 0 selects a free port. No game launch or routing changes.");
    Console.WriteLine("--profile-dir keeps a persistent local profile (requires startup fixtures); a new one starts from fixture defaults.");
    Console.WriteLine("--live-war <cache file> follows the live galactic war read-only through the community mirror " + LiveWar.Mirror + " (fetched before ready, then every 5 minutes; the cache is used offline).");
    Console.WriteLine("HellForge.LocalBackend fetch-live-war <cache file>   fetches the live war once into the cache file, for a session with no network (HELLFORGE_LINUX_NETNS=1 serves it without refreshing).");
    Console.WriteLine("--war open serves the all-planets war instead: the frozen war with every planet but Super Earth an enemy's, each with a liberation campaign (no --live-war).");
    Console.WriteLine("--pipe <name> serves the game's lobby stand-in on that named pipe; --link none|steam:<steam_api64.dll>|udp:<id>:<port>:<peer>=<port>,... reaches friends' backends; --mod-digest <text> must match theirs; --mod-list <file> names its mods, so a refused join says which differ; --modset <file> is the session's mod set, which joiners lacking its mods may download; --directory <folder> lists public lobbies in a folder (the loopback harness; Steam's directory otherwise); --rejoin <file> (after a mod sync, a ModSyncSession) invites the game back into that host's lobby once it is on the ship.");
    return 0;
}
if (args is ["profile", "show", var showDirectory])
{
    try
    {
        // Validates without taking the lease, so it also works while a server is running.
        Console.WriteLine(ProfileStore.Load(Path.Combine(showDirectory, ProfileStore.FileName)).ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }
    catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
    {
        Console.Error.WriteLine($"HellForge local backend: {error.Message}");
        return 1;
    }
}
if (args is ["profile", var templateCommand and ("apply-template" or "add-template-items"), var templateDirectory, var templateName])
{
    try
    {
        using var store = ProfileStore.Open(templateDirectory);
        if (templateCommand == "add-template-items")
        {
            Console.WriteLine($"Added {store.AddMissingTemplateItems(templateName)} items from template '{templateName}' to {store.FilePath}");
            return 0;
        }
        store.ApplyTemplate(templateName);
        Console.WriteLine($"Applied template '{templateName}' to {store.FilePath}");
        return 0;
    }
    catch (Exception error) when (error is ArgumentException or InvalidDataException or IOException or UnauthorizedAccessException)
    {
        Console.Error.WriteLine($"HellForge local backend: {error.Message}");
        return 1;
    }
}
if (args is ["profile", var accountCommand and ("import-preview" or "import"), var accountDirectory, .. var accountTemplate] && accountTemplate.Length <= 1)
    return accountCommand == "import"
        ? AccountCommands.Import(accountDirectory, accountTemplate.FirstOrDefault(), HellForge.Core.SteamAccount.Current(), Console.Out, Console.Error)
        : AccountCommands.Preview(accountDirectory, accountTemplate.FirstOrDefault(), HellForge.Core.SteamAccount.Current(), Console.Out, Console.Error);
if (args is ["profile", "restore", var restoreDirectory, var backupFile])
    return AccountCommands.Restore(restoreDirectory, backupFile, Console.Out, Console.Error);
if (args is ["fetch-live-war", var fetchCache])
    return await LiveWar.FetchOnceAsync(fetchCache, Console.WriteLine) ? 0 : 1;
try
{
    var options = BackendOptions.Parse(args);
    if (!int.TryParse(options.GetValueOrDefault("--port", "0"), out var port) || port is < 0 or > 65535)
        throw new ArgumentException("Port must be between 0 and 65535.");
    var mode = options.GetValueOrDefault("--mode", "local-session") switch
    {
        "local-session" => LoginMode.LocalSession,
        "reject-gmi" => LoginMode.RejectGmi,
        _ => throw new ArgumentException("Mode must be local-session or reject-gmi.")
    };
    var evidence = Path.GetFullPath(options.GetValueOrDefault("--evidence") ??
        Path.Combine("artifacts", "local-backend", Guid.NewGuid().ToString("N")));
    if (!bool.TryParse(options.GetValueOrDefault("--startup-fixtures", "false"), out var startupFixtures))
        throw new ArgumentException("Startup fixtures must be true or false.");
    if (!int.TryParse(options.GetValueOrDefault("--lifetime-seconds", "0"), out var lifetimeSeconds) ||
        lifetimeSeconds is < 0 or > 300)
        throw new ArgumentException("Lifetime must be between 0 (unlimited) and 300 seconds.");
    var profileDirectory = options.GetValueOrDefault("--profile-dir");
    if (profileDirectory != null && !startupFixtures) throw new ArgumentException("--profile-dir requires --startup-fixtures true.");
    using var lifetime = new CancellationTokenSource();
    if (lifetimeSeconds > 0) lifetime.CancelAfter(TimeSpan.FromSeconds(lifetimeSeconds));
    var profileTemplate = options.GetValueOrDefault("--profile-template");
    if (profileTemplate != null && !startupFixtures) throw new ArgumentException("--profile-template requires --startup-fixtures true.");
    using var profile = startupFixtures ? ProfileStore.Open(profileDirectory, profileTemplate) : null;
    // Premium items (OwnershipFilter): served only from the profile's account snapshot, when it is sealed on this PC for
    // the Steam account signed in now. The rest of a snapshot reaches the profile only through an import the player
    // chose (AccountCommands), never at start.
    if (profile != null)
    {
        var snapshotDir = profileDirectory != null ? HellForge.Core.InventorySync.SnapshotDirectoryFromProfileDir(profileDirectory) : null;
        profile.Ownership = OwnershipFilter.ForSnapshot(PaidContentSet.Load(), snapshotDir, HellForge.Core.SteamAccount.Current(), out var refusal);
        Console.Error.WriteLine(refusal != null ? "[premium] none unlocked: " + refusal
            : "[premium] " + profile.Ownership.OwnedPaidCount + " premium items from the account snapshot");
    }
    var warClock = new WarClock();
    var accountKeys = new AccountKeys();
    var presence = new FriendPresence();
    // --sandbox: the session's game runs at low integrity, so it writes only into the run's game folder (PlaySession's
    // gameOut), and the pipes it opens carry a low label (PipeServer)
    var sandboxed = options.GetValueOrDefault("--sandbox") == "true";
    var registeredItems = new RegisteredItems(RegisteredItems.PathFor(evidence, sandboxed));
    // Premium content mods build on: a copy of a premium base follows the player's ownership, and the mods mods-stage
    // locked (--premium-locked: their declared premium list is not owned) add nothing to the inventory.
    if (profile?.Ownership is { } ownership) registeredItems.OwnsBase = ownership.IsAllowed;
    registeredItems.LockedMods = HellForge.Core.PremiumMods.ReadLocked(options.GetValueOrDefault("--premium-locked"));
    // --live-war <cache file>: follow the live galactic war read-only through the community mirror (LiveWar).
    Directory.CreateDirectory(evidence);
    var liveWarLog = Path.Combine(evidence, "live-war.log");
    // --war open: the all-planets war (OpenWar); it takes the place of the live war.
    var war = options.GetValueOrDefault("--war");
    if (war != null && war != OpenWar.Mode) throw new ArgumentException("--war takes \"" + OpenWar.Mode + "\".");
    var openWar = war == OpenWar.Mode;
    // --war-seed <n>: the all-planets war's subfaction seed (0 the default layout)
    var warSeed = options.GetValueOrDefault("--war-seed") is { } seedText
        ? uint.TryParse(seedText, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var parsed) ? parsed
            : throw new ArgumentException("--war-seed takes a whole number from 0 to 4294967295.")
        : 0u;
    // --war-subfactions / --war-seaf yes|no: whether it deals its subfactions and SEAF allies from the seed (yes, the
    // default) or keeps the frozen war's
    bool Deal(string name) => options.GetValueOrDefault(name) switch
    {
        null or "yes" => true,
        "no" => false,
        _ => throw new ArgumentException(name + " takes yes or no."),
    };
    var dealSubfactions = Deal("--war-subfactions");
    var dealSeaf = Deal("--war-seaf");
    await using var liveWar = !openWar && options.GetValueOrDefault("--live-war") is { } liveWarCache && startupFixtures
        ? await LiveWar.StartAsync(liveWarCache, warClock, message =>
        {
            lock (liveWarLog) File.AppendAllText(liveWarLog, DateTime.UtcNow.ToString("HH:mm:ss.fff", System.Globalization.CultureInfo.InvariantCulture) + " " + HellForge.Core.Privacy.Scrub(message) + Environment.NewLine);
        }, cacheOnly: Environment.GetEnvironmentVariable("HELLFORGE_LINUX_NETNS") == "1") // Linux: fetched before the namespace
        : null;
    await using var app = LocalBackend.Create(new BackendSettings(port, evidence, mode)
    {
        StartupFixtures = startupFixtures, Profile = profile, ItemCatalog = options.GetValueOrDefault("--item-catalog"),
        FixtureOverride = options.GetValueOrDefault("--fixture-override"), CaptureMissionBodies = options.GetValueOrDefault("--capture-mission-bodies") == "true",
        WarClock = warClock, AccountKeys = accountKeys, LiveWar = liveWar, Presence = presence, RegisteredItems = registeredItems, OpenWar = openWar, WarSeed = warSeed,
        DealSubfactions = dealSubfactions, DealSeaf = dealSeaf
    });
    await app.StartAsync(lifetime.Token);
    var pipeName = options.GetValueOrDefault("--pipe");
    HellForge.Core.PipeServer.LowIntegrityClients = sandboxed;
    // --mod-list <file>: the digest's mods one by one (ModlistDiff's form), so a refused join can say which differ
    var modList = options.GetValueOrDefault("--mod-list") is { } modListFile ? File.ReadAllText(modListFile) : null;
    await using var multiplayer = pipeName != null
        ? HellForge.LocalBackend.Multiplayer.MultiplayerHost.Start(new(pipeName, options.GetValueOrDefault("--link", "none"), options.GetValueOrDefault("--mod-digest", "none"), evidence)
        {
            WarClock = warClock, AccountKeys = accountKeys, Presence = presence, ModsFolder = options.GetValueOrDefault("--mods"), ModList = modList,
            ModSetFile = options.GetValueOrDefault("--modset"), DirectoryFolder = options.GetValueOrDefault("--directory"), RejoinFile = options.GetValueOrDefault("--rejoin"),
            SandboxStats = sandboxed ? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(evidence))!, "game", "sandbox-stats") : null
        })
        : null;
    var ready = new System.Text.Json.Nodes.JsonObject
    {
        ["scope"] = "synthetic-local-backend", ["urls"] = new System.Text.Json.Nodes.JsonArray(app.Urls.Select(u => (System.Text.Json.Nodes.JsonNode?)u).ToArray()),
        ["mode"] = mode.ToString(), ["evidence"] = evidence, ["startupFixtures"] = startupFixtures,
        ["profile"] = profile?.FilePath, ["profileCreated"] = profile?.Created, ["lifetimeSeconds"] = lifetimeSeconds
    };
    var json = ready.ToJsonString();
    await File.WriteAllTextAsync(Path.Combine(evidence, "ready.json"), json);
    Console.WriteLine(json);
    await app.WaitForShutdownAsync(lifetime.Token);
    await File.WriteAllTextAsync(Path.Combine(evidence, "stopped.json"),
        new System.Text.Json.Nodes.JsonObject { ["stoppedUtc"] = DateTimeOffset.UtcNow,
            ["reason"] = lifetime.IsCancellationRequested ? "lifetime_elapsed" : "shutdown_requested" }.ToJsonString());
    return 0;
}
catch (Exception error) when (error is ArgumentException or InvalidDataException or IOException or UnauthorizedAccessException)
{
    Console.Error.WriteLine($"HellForge local backend: {error.Message}");
    return 1;
}
