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
            : options.TryGetValue("--all", out var all) ? WeaponLevels.All(int.TryParse(all, out var level) ? level : -1, weapons)
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
    static string? SendModSyncAnswer(Dictionary<string, string> options, string? backend, ModSyncOffer? offer)
    {
        if (!options.TryGetValue("--answer", out var answer))
        {
            if (options.ContainsKey("--version") || options.ContainsKey("--mod")) throw new ArgumentException("--version and --mod go with --answer.");
            return null;
        }
        if (offer == null || backend == null) throw new InvalidDataException("No mod sync offer is waiting.");
        if (!int.TryParse(options.GetValueOrDefault("--version"), out var version))
            throw new ArgumentException("--answer needs the offer's --version (mod-sync-offer prints it).");
        if (version != offer.Version) throw new InvalidDataException($"The offer has changed (it is version {offer.Version} now): read it again before answering.");
        ModSyncOffer.Answer(backend, version, answer, options.GetValueOrDefault("--mod"));
        return answer;
    }

    // mod-sync-offer: the running session's MOD SYNC offer (<run>/backend/mod-sync.json), as the launcher's Play page card
    // shows it, with the answers its buttons send and each mod's TRUST warning. --answer <answer> --version <offer version>
    // [--mod <mod id>] sends the player's choice, which the session takes within a second. offer is null when none waits.
    public static object ModSyncOfferCommand(Dictionary<string, string> options)
    {
        var install = Install(options);
        var paths = new InstallSessionPaths(install);
        var run = options.TryGetValue("--run", out var given) ? Path.GetFullPath(given) : SessionRunner.LatestRun(paths);
        if (run != null && (!LocalInstall.Within(run, install.Sessions) || !Directory.Exists(run)))
            throw new InvalidDataException($"{run} is not a session of this installation.");
        var backend = run != null ? Path.Combine(run, "backend") : null;
        var offer = backend != null && SessionRunning() && ModSyncOffer.Read(backend) is { State: not ("none" or "") } waiting ? waiting : null;
        var answer = SendModSyncAnswer(options, backend, offer);
        return new
        {
            run,
            // the editor mod is on, so the game shows the prompt itself (a session keeps the mods it started with)
            inGamePrompt = ModCatalog.Load(install.Mods).Mods.Any(m => m.Id == "hellforge.editor" && m.Enabled),
            sent = answer,
            offer = offer == null ? null : new
            {
                offer.Version, offer.State, offer.Host, offer.Stranger, offer.Live, offer.Bytes, offer.Done, offer.Message,
                answers = offer.Choices,
                mods = offer.Mods.Select(m => new
                {
                    m.Id, m.Name, m.Version, @class = m.Class, m.Action, m.Bytes, m.Url,
                    trustWarning = m.Action == "trust" ? offer.TrustWarning(m) : null
                }),
                trustNote = offer.TrustNote
            }
        };
    }
}
