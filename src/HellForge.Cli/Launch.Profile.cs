using System.Text.Json;
using HellForge.Core;

// The two things HellForge.exe offered that other launchers could not reach: the profile page's weapon levels, and the
// answer to the MOD SYNC prompt when the game has no in-game editor to show it (docs/external-launchers.md).
static partial class Launch
{
    // weapon-levels: the profile page's Weapon levels card for a profile (the selected one, or --profile): each weapon's
    // mastery track and level. --set <package>=<level>,... or --all <level> changes them between sessions, as SAVE LEVELS
    // and SET ALL do; the next session uses them.
    public static object WeaponLevelsCommand(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var name = options.GetValueOrDefault("--profile") ?? ProfileStore.LoadSelected(paths);
        var profiles = ProfileStore.List(paths);
        var profile = profiles.FirstOrDefault(p => p.Name == name) ?? throw new ArgumentException(
            $"No profile named {name}; there are: {string.Join(", ", profiles.Select(p => p.Name))}.");
        if (options.ContainsKey("--set") && options.ContainsKey("--all")) throw new ArgumentException("Give --set or --all, not both.");
        var weapons = WeaponLevels.Load(paths.ProgressionFile, paths.RunsFolder);
        var data = Used(profile);
        var change = options.TryGetValue("--set", out var set) ? WeaponLevels.Parse(set, weapons)
            : options.TryGetValue("--all", out var all) ? WeaponLevels.All(WeaponLevels.ParseLevel(all), weapons)
            : null;
        if (change != null)
        {
            if (SessionRunning()) throw new InvalidDataException("A HellForge session is running: set weapon levels once it has ended.");
            if (data == null) throw new InvalidDataException($"{name} has not been used yet: the backend fills it on its first session, then its levels can be set.");
            if (WeaponLevels.Save(paths.ProgressionFile, profile.ProfileFile, change, weapons.Where(w => w.Mod).Select(w => w.Package).ToHashSet()) is { } error)
                throw new InvalidDataException(error);
            data = Used(profile);
        }
        var levels = data is { } saved ? WeaponLevels.Levels(saved) : [];
        return new
        {
            profile = name,
            used = data != null,
            weapons = weapons.Select(w => new
            {
                w.Package, weapon = w.Weapon.ToString("X8"), w.Name, w.MaxLevel, level = Math.Min(levels.GetValueOrDefault(w.Package), w.MaxLevel), w.Mod
            })
        };
    }

    // The profile's profile.json once a session has filled it, as the profile page reads it; null before that.
    static JsonElement? Used(LocalProfile profile) =>
        SessionFiles.ReadJson(profile.ProfileFile) is { ValueKind: JsonValueKind.Object } data &&
        data.TryGetProperty("progression", out var progression) && progression.ValueKind == JsonValueKind.Object ? data : null;

    // mod-sync-offer --answer: the player's choice for the offer they were shown (its --version), written for the session;
    // the answer sent, or null without --answer.
    // It is refused while the game shows the prompt itself (ModSyncRun.CheckAnswer).
    static string? SendModSyncAnswer(Dictionary<string, string> options, string? run, ModSyncOffer? offer, bool inGamePrompt)
    {
        if (!options.TryGetValue("--answer", out var answer))
        {
            if (options.ContainsKey("--version") || options.ContainsKey("--mod")) throw new ArgumentException("--version and --mod go with --answer.");
            return null;
        }
        var mod = options.GetValueOrDefault("--mod");
        var version = ModSyncRun.CheckAnswer(run == null ? null : offer, inGamePrompt, answer, options.GetValueOrDefault("--version"), mod);
        ModSyncOffer.Answer(Path.Combine(run!, "backend"), version, answer, mod);
        return answer;
    }

    // Whether that process id is a running game (the run's gamePid). Asked on Windows only: under Wine the id is not the host's.
    static bool GameAlive(int pid)
    {
        try
        {
            using var process = System.Diagnostics.Process.GetProcessById(pid);
            return !process.HasExited;
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { return false; }
    }

    // mod-sync-offer: the running session's MOD SYNC offer (<run>/backend/mod-sync.json), as the launcher's Play page card
    // shows it, with the answers its buttons send and each mod's TRUST warning. --answer <answer> --version <offer version>
    // [--mod <mod id>] sends the player's choice, which the session takes within a second. offer is null when none waits:
    // an offer left in a run that ended, or whose game is gone, does not wait, whatever other session runs on the PC.
    // Launchers poll this: it reads the run's own files (ModSyncRun) and never loads the mods.
    public static object ModSyncOfferCommand(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var run = options.TryGetValue("--run", out var given) ? Path.GetFullPath(given) : SessionRunner.LatestRun(paths);
        if (run != null && (!LocalInstall.Within(run, install.Sessions) || !Directory.Exists(run)))
            throw new InvalidDataException($"{run} is not a session of this installation.");
        var offer = ModSyncRun.Waiting(run, SessionRunning(), OperatingSystem.IsWindows() ? GameAlive : null);
        // the editor is one of the session's mods, so the game shows the prompt itself (a session keeps the mods it started with)
        var inGamePrompt = ModSyncRun.InGamePrompt(run, install.Mods);
        var answer = SendModSyncAnswer(options, run, offer, inGamePrompt);
        return new
        {
            run,
            inGamePrompt,
            sent = answer,
            offer = offer == null ? null : new
            {
                offer.Version, offer.State, offer.Host, offer.Stranger, offer.Live, offer.Bytes, offer.Done, offer.Message,
                answers = offer.Choices,
                mods = offer.Mods.Select(m => new
                {
                    m.Id, m.Name, m.Version, @class = m.Class, m.Action, m.Bytes, m.Url,
                    answers = offer.ModChoices(m),
                    trustWarning = m.Action == "trust" ? offer.TrustWarning(m) : null
                }),
                trustNote = offer.TrustNote
            }
        };
    }
}
