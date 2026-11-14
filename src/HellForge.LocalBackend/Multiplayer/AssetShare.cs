using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// Assets in co-op (docs/multiplayer.md "Assets in co-op"). Every player keeps their own edits, and what a friend's edit
// puts in the game (a vehicle's swapped weapon, a pod's pickup, a swapped round's effects) is made on every machine, which
// needs its package loaded or shows a missing asset. Each game's mod loader says which packages it holds for its own
// edits, mods and actions (the live pipe's "packages" line, native/mod-loader/packages.c); this gathers the lobby's and
// has every game load the others'.
//   joiner: its game's packages go to the host of the lobby it joined, numbered, whenever they change:
//           "assets.mine" [version, ids].
//   host:   keeps each friend's, and sends the lobby's (its own and every friend's) to every friend, numbered, whenever
//           they change: "assets.lobby" [version, ids, the version of that friend's own it holds (-1 none): a joiner whose
//           own the host lacks, or has an older one of, sends them again].
//   every game is sent the lobby's packages less its own ("preload"), again whenever they change; the loader asks for
//   each as it does for its own edits' (catalogue packages only, at most 48 of a friend's: 16 of its 64 stay its own).
// Ids are package ids, 16 hex digits each, comma separated ("-" none): at most 64 a player and 256 a lobby. Packages are
// never released (packages.c), so a player's change taken back leaves its package loaded for the session.
// Shots in co-op (docs/multiplayer.md "Shots in co-op"): each player's projectile swaps (their Local Stats' weapons'
// ProjectileWeapon +0) travel the same way, "<weapon resource hex>=<type>" in "assets.mine" and, for every player of
// the lobby, "<peer hex>:<weapon resource hex>=<type>" in "assets.lobby"; each game is sent the lobby's ("shots"), so its
// copies of a player's shots fire that player's round (native/mod-loader/shots.c). Not while the host shares its stats:
// then every machine plays with the same.
// Rounds in co-op: each player's changes of projectile and damage rows (their Local Stats' ProjectileSettings and
// DamageInfo values: flight, armour penetration, damage) travel beside the swaps, "P<type>+<offset hex>=<bits hex>" and
// "D<id>+<offset hex>=<bits hex>" semicolon separated in "assets.mine" and, for every player of the lobby,
// "<peer hex>=<entries>" comma separated in "assets.lobby"; each game is sent the lobby's ("rounds"), so its copies of a
// player's shots fly and hit with that player's values (native/mod-loader/rounds.c). Older backends send neither.
// Deployables in co-op: each player's changes of the Shield, Health, OrbitalAbility and Bombardment records of what they
// call in travel the same way, "E<resource hex>/<S|H|O|B>+<offset hex>=<bits hex>" semicolon separated, and each game is
// sent the lobby's ("deploys"), so its copies of a player's relays, sentries and orbitals carry that player's values
// (native/mod-loader/deploys.c).
public sealed class AssetShare : IAsyncDisposable
{
    public const int MaxPerPlayer = 64, MaxLobby = 256, MaxRounds = 96, MaxRoundsText = 7000, MaxDeploys = 128;
    readonly IPeerTransport transport;
    readonly LobbyService lobby;
    readonly LiveEditServer live;
    readonly Action<string> log;
    readonly object gate = new();
    readonly Timer timer;
    readonly SemaphoreSlim preloading = new(1, 1);
    // this game's own packages and their version
    SortedSet<ulong> mine = [];
    long mineVersion;
    // joiner: the host this game's were last sent to, and their version then
    ulong sentHost;
    long sentVersion = -1;
    // host: each friend's own and their version; the lobby's and its version; the version each friend has
    readonly Dictionary<ulong, (long Version, SortedSet<ulong> Ids, string Swaps, string Rounds, string Deploys)> friends = [];
    SortedSet<ulong> lobbyIds = [];
    string lobbySwaps = "-", lobbyRounds = "-", lobbyDeploys = "-";
    long lobbyVersion;
    readonly Dictionary<ulong, long> lobbySent = [];
    // joiner: the lobby's packages from its host, and their version
    ulong lobbyHost;
    long lobbyHostVersion = -1;
    SortedSet<ulong> hostLobbyIds = [];
    string hostLobbySwaps = "-", hostLobbyRounds = "-", hostLobbyDeploys = "-";
    // the friends' packages this game was last asked to load ("" none yet), and the lobby's swaps and rounds it was last sent
    string preloaded = "", shotsSent = "", roundsSent = "", deploysSent = "";
    // this game's own projectile swaps and round changes (from its Local Stats file, as last read)
    string mineSwaps = "-", mineRounds = "-", mineDeploys = "-";
    string? swapsFrom;
    readonly Func<string?> localStats;
    readonly Func<bool> sharedStats;
    readonly Action stateChanged;

    // localStats: this machine's Local Stats file as StatsShare last read it; sharedStats: whether the lobby plays with
    // its host's stats (StatsShare.LobbyPlaysHostStats).
    public AssetShare(IPeerTransport transport, LobbyService lobby, LiveEditServer live, Action<string> log, Func<string?>? localStats = null,
        Func<bool>? sharedStats = null)
    {
        this.transport = transport;
        this.lobby = lobby;
        this.live = live;
        this.log = log;
        this.localStats = localStats ?? (() => null);
        this.sharedStats = sharedStats ?? (() => false);
        live.Received += FromGame;
        stateChanged = () => { lock (gate) preloaded = shotsSent = roundsSent = deploysSent = ""; Tick(); }; /* a game that connects again is asked again */
        live.StateChanged += stateChanged;
        timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    static string List(IEnumerable<ulong> ids)
    {
        var text = string.Join(',', ids.Select(id => id.ToString("x16", System.Globalization.CultureInfo.InvariantCulture)));
        return text.Length == 0 ? "-" : text;
    }

    // A player's projectile swaps from their Local Stats file: "<weapon resource hex>=<type>[/r|/f|/<type>]" comma
    // separated, "-" none. The weapon is the one whose shots it changes (an ammunition's swap: the weapon it belongs to),
    // and the suffix tells the loader where that weapon's round is (StatsSchema.ProjectileSwaps); a programmable
    // round ("/f") is a swap of its own beside the weapon's normal one.
    public static string Swaps(string? stats)
    {
        if (string.IsNullOrWhiteSpace(stats)) return "-";
        try
        {
            var sites = StatsSchema.Current.ProjectileSwaps;
            var swaps = StatsData.Parse(stats, EntityProfile.Current)
                .Where(e => !e.Own && e.Value != 0 && e.Value <= 0xFFFF)
                .SelectMany(e => sites.TryGetValue(StatsSchema.SwapSite(e.Kind, e.Component, e.Resource, e.Offset), out var list)
                    ? list.Select(site => (Edit: e, Site: site)) : [])
                // one swap per round of a weapon: its slot ("" / "/r" are its normal round, "/f" its programmable one, a
                // stock type that round), the last edit winning
                .GroupBy(s => (s.Site.Weapon, Round: s.Site.Native is "" or "/r" ? "" : s.Site.Native)).Select(g => g.Last())
                .OrderBy(s => s.Site.Weapon).ThenBy(s => s.Site.Native, StringComparer.Ordinal)
                .Select(s => s.Site.Weapon.ToString("x", System.Globalization.CultureInfo.InvariantCulture) + "=" +
                    s.Edit.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) + s.Site.Native)
                .Take(MaxPerPlayer);
            var text = string.Join(',', swaps);
            return text.Length == 0 ? "-" : text;
        }
        catch (Exception error) when (error is InvalidDataException or System.Text.Json.JsonException) { return "-"; }
    }

    // A player's changes of projectile and damage rows from their Local Stats file: "P<type>+<offset hex>=<bits hex>"
    // (ProjectileSettings) or "D<id>+<offset hex>=<bits hex>" (DamageInfo) semicolon separated, at most MaxRounds, "-"
    // none. Four-byte fields only, never a row's own id at +0 (the loader keeps the stock type).
    public static string Rounds(string? stats)
    {
        if (string.IsNullOrWhiteSpace(stats)) return "-";
        try
        {
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            var entries = StatsData.Parse(stats, EntityProfile.Current)
                .Where(e => !e.Own && (e.Kind is "ProjectileSettings" or "DamageInfo") && e.Record > 0 && e.Offset > 0 && e.Offset % 4 == 0 &&
                    (e.Storage is "u32" or "f32" or "i32"))
                .GroupBy(e => (e.Kind, e.Record, e.Offset)).Select(g => g.Last())
                .OrderBy(e => e.Kind, StringComparer.Ordinal).ThenBy(e => e.Record).ThenBy(e => e.Offset)
                .Select(e => (e.Kind == "DamageInfo" ? "D" : "P") + e.Record.ToString(inv) + "+" + e.Offset.ToString("x", inv) + "=" + e.Value.ToString("x", inv))
                .Take(MaxRounds);
            var text = string.Join(';', entries);
            return text.Length == 0 ? "-" : text;
        }
        catch (Exception error) when (error is InvalidDataException or System.Text.Json.JsonException) { return "-"; }
    }

    // The records deploys.c gives a private copy, by the letter that names them.
    static readonly Dictionary<string, char> DeployRecords = new(StringComparer.Ordinal)
    {
        ["ShieldComponentData"] = 'S', ["HealthComponentData"] = 'H', ["OrbitalAbilityComponentData"] = 'O', ["BombardmentComponentData"] = 'B',
    };

    // The entities a player calls in, as the stats schema has them, so a change of their Helldiver's or a weapon's Health
    // stays out of the list.
    static readonly Lazy<HashSet<ulong>> Deployables = new(() =>
        new[] { "stratagems", "backpacks" }.SelectMany(c => StatsSchema.Current.Targets.GetValueOrDefault(c)?.Values ?? Enumerable.Empty<StatsTarget>())
            .SelectMany(t => t.Fields.Values).Where(f => f.Kind == "component" && DeployRecords.ContainsKey(f.Component))
            .Select(f => Convert.ToUInt64(f.Resource, 16)).Where(r => r != 0).ToHashSet());

    // A player's changes of those records from their Local Stats file: "E<resource hex>/<S|H|O|B>+<offset hex>=<bits hex>"
    // semicolon separated, at most MaxDeploys, "-" none. Four-byte fields.
    public static string Deploys(string? stats)
    {
        if (string.IsNullOrWhiteSpace(stats)) return "-";
        try
        {
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            var entries = StatsData.Parse(stats, EntityProfile.Current)
                .Where(e => !e.Own && e.Kind == "component" && DeployRecords.ContainsKey(e.Component) && Deployables.Value.Contains(e.Resource) &&
                    e.Offset >= 0 && e.Offset % 4 == 0 && (e.Storage is "u32" or "f32" or "i32"))
                .GroupBy(e => (e.Resource, e.Component, e.Offset)).Select(g => g.Last())
                .OrderBy(e => e.Resource).ThenBy(e => DeployRecords[e.Component]).ThenBy(e => e.Offset)
                .Select(e => "E" + e.Resource.ToString("x", inv) + "/" + DeployRecords[e.Component] + "+" + e.Offset.ToString("x", inv) + "=" + e.Value.ToString("x", inv))
                .Take(MaxDeploys);
            var text = string.Join(';', entries);
            return text.Length == 0 ? "-" : text;
        }
        catch (Exception error) when (error is InvalidDataException or System.Text.Json.JsonException) { return "-"; }
    }

    // Whether a rounds list is well formed: "-", or entries (with peers: "<peer hex>=<entries>" comma separated).
    static readonly System.Text.RegularExpressions.Regex RoundEntries =
        new(@"^[PD][0-9]{1,4}\+[0-9a-f]{1,3}=[0-9a-f]{1,8}(;[PD][0-9]{1,4}\+[0-9a-f]{1,3}=[0-9a-f]{1,8})*$");
    static bool WellFormedRounds(string list, bool peers) => list == "-" || list.Length <= MaxRoundsText &&
        (peers ? list.Split(',').All(g => g.IndexOf('=') is var at and > 0 and <= 16 && g[..at].All(Uri.IsHexDigit) && RoundEntries.IsMatch(g[(at + 1)..]))
            : RoundEntries.IsMatch(list));

    // Whether a deploys list is well formed: "-", or entries (with peers: "<peer hex>=<entries>" comma separated).
    static readonly System.Text.RegularExpressions.Regex DeployEntries =
        new(@"^E[0-9a-f]{1,16}/[SHOB]\+[0-9a-f]{1,4}=[0-9a-f]{1,8}(;E[0-9a-f]{1,16}/[SHOB]\+[0-9a-f]{1,4}=[0-9a-f]{1,8})*$");
    static bool WellFormedDeploys(string list, bool peers) => list == "-" || list.Length <= MaxRoundsText &&
        (peers ? list.Split(',').All(g => g.IndexOf('=') is var at and > 0 and <= 16 && g[..at].All(Uri.IsHexDigit) && DeployEntries.IsMatch(g[(at + 1)..]))
            : DeployEntries.IsMatch(list));

    // Whether a swaps list is well formed: "-", or "<hex>=<type>[/r|/f|/<type>]" ("<hex>:<hex>=..." with peers) comma
    // separated, at most max.
    static bool WellFormed(string list, bool peers, int max) => list == "-" || list.Split(',') is var parts && parts.Length <= max &&
        parts.All(p => System.Text.RegularExpressions.Regex.IsMatch(p, (peers ? "^[0-9a-f]{1,16}:" : "^") + "[0-9a-f]{1,16}=[0-9]{1,5}(/r|/f|/[0-9]{1,5})?$"));

    // This game's "packages" line: its own packages, which friends' games load.
    void FromGame(string[] parts)
    {
        if (parts[0] != "packages") return;
        var ids = parts.Length == 2 ? LiveEditServer.Packages(parts[1], MaxPerPlayer) : null;
        if (ids == null) { log("Assets: the game sent a malformed list of its packages."); return; }
        lock (gate)
        {
            if (ids.SetEquals(mine)) return;
            (mine, mineVersion) = (ids, mineVersion + 1);
        }
        log($"Assets: this game holds {ids.Count} package(s) for its own changes, mods and actions.");
        Tick();
    }

    // A friend's backend: a joiner's own packages (to the host of its lobby), or the lobby's (from the host of the lobby
    // this machine joined).
    public void FromPeer(ulong peer, Frame frame)
    {
        if (frame.Count < 2 || !long.TryParse(frame.Text(0), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var version)) return;
        if (frame.Type == "assets.mine" && lobby.InHostedLobby(peer)) FromJoiner(peer, frame, version);
        else if (frame.Type == "assets.lobby" && lobby.HostsJoinedLobby(peer)) FromHost(peer, frame, version);
    }

    // A joiner's own packages, to this host of its lobby.
    void FromJoiner(ulong peer, Frame frame, long version)
    {
        var ids = LiveEditServer.Packages(frame.Text(1), MaxPerPlayer);
        var swaps = frame.Count > 2 ? frame.Text(2) : "-";
        var rounds = frame.Count > 3 ? frame.Text(3) : "-";
        var deploys = frame.Count > 4 ? frame.Text(4) : "-";
        if (ids == null || !WellFormed(swaps, false, MaxPerPlayer) || !WellFormedRounds(rounds, false) || !WellFormedDeploys(deploys, false))
        { log($"Assets: {LobbyService.EntityId(peer)} sent a malformed list of its packages."); return; }
        lock (gate)
        {
            if (friends.TryGetValue(peer, out var had) && version <= had.Version) return;
            friends[peer] = (version, ids, swaps, rounds, deploys);
        }
        log($"Assets: {LobbyService.EntityId(peer)}'s game holds {ids.Count} package(s) for its own changes (version {version}).");
        Tick();
    }

    // The lobby's packages, from the host of the lobby this machine joined.
    void FromHost(ulong peer, Frame frame, long version)
    {
        var ids = LiveEditServer.Packages(frame.Text(1), MaxLobby);
        var swaps = frame.Count > 3 ? frame.Text(3) : "-";
        var rounds = frame.Count > 4 ? frame.Text(4) : "-";
        var deploys = frame.Count > 5 ? frame.Text(5) : "-";
        if (ids == null || !WellFormed(swaps, true, MaxLobby) || !WellFormedRounds(rounds, true) || !WellFormedDeploys(deploys, true)) { log($"Assets: the host {LobbyService.EntityId(peer)} sent a malformed list of the lobby's packages."); return; }
        var held = frame.Count > 2 && long.TryParse(frame.Text(2), System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out var h) ? h : -1;
        bool resend, fresh;
        lock (gate)
        {
            resend = peer == sentHost && held != sentVersion; /* the host lacks this game's latest: sent again */
            if (resend) sentVersion = -1;
            fresh = peer != lobbyHost || version > lobbyHostVersion;
            if (fresh) (lobbyHost, lobbyHostVersion, hostLobbyIds, hostLobbySwaps, hostLobbyRounds, hostLobbyDeploys) = (peer, version, ids, swaps, rounds, deploys);
        }
        if (fresh) log($"Assets: the lobby's packages from the host {LobbyService.EntityId(peer)}: {ids.Count} (version {version}).");
        if (fresh || resend) Tick();
    }

    // Every second and on every change: this game's packages to its host, the lobby's to friends, and the friends' to this
    // game.
    void Tick()
    {
        try
        {
            var hosts = lobby.JoinedHosts();
            var hosted = lobby.HostedFriends();
            var sends = new List<(ulong To, Frame Frame, string What)>();
            SortedSet<ulong> want;
            string shots, rounds, deploys;
            var stats = localStats();
            var shared = sharedStats();
            lock (gate)
            {
                ReadOwnSwaps(stats);
                SendOwn(hosts, sends);
                SendLobby(hosted, sends);
                // this game loads what the lobby's other players hold
                want = new SortedSet<ulong>((hosted.Count > 0 ? lobbyIds : []).Concat(hostLobbyIds).Where(id => !mine.Contains(id)));
                // whose swaps this game's copies of each player's shots fire: none while the host shares its stats, or alone
                shots = shared ? "-" : hosted.Count > 0 ? lobbySwaps : lobbyHost != 0 ? hostLobbySwaps : "-";
                rounds = shared ? "-" : hosted.Count > 0 ? lobbyRounds : lobbyHost != 0 ? hostLobbyRounds : "-";
                deploys = shared ? "-" : hosted.Count > 0 ? lobbyDeploys : lobbyHost != 0 ? hostLobbyDeploys : "-";
            }
            foreach (var (to, frame, what) in sends)
            {
                transport.Send(to, frame.Encode(), reliable: true);
                log($"Assets: sent {what} to {LobbyService.EntityId(to)}.");
            }
            _ = PreloadAsync(want);
            _ = ShotsAsync(shots);
            _ = RoundsAsync(rounds);
            _ = DeploysAsync(deploys);
        }
        catch (Exception error) { log("Assets: sharing failed: " + error.Message); }
    }

    // this game's own projectile swaps and round changes: read again when its Local Stats changed. Called with gate held.
    void ReadOwnSwaps(string? stats)
    {
        if (stats == swapsFrom) return;
        swapsFrom = stats;
        var swaps = Swaps(stats);
        if (swaps != mineSwaps) (mineSwaps, mineVersion) = (swaps, mineVersion + 1);
        var changes = Rounds(stats);
        if (changes != mineRounds) (mineRounds, mineVersion) = (changes, mineVersion + 1);
        var deploys = Deploys(stats);
        if (deploys != mineDeploys) (mineDeploys, mineVersion) = (deploys, mineVersion + 1);
    }

    // joined: this game's own to the host (again to a new host). Called with gate held.
    void SendOwn(IReadOnlyCollection<ulong> hosts, List<(ulong To, Frame Frame, string What)> sends)
    {
        var host = hosts.Count > 0 ? hosts.Min() : 0;
        if (host != 0 && (host != sentHost || sentVersion != mineVersion))
        {
            sends.Add((host, new Frame("assets.mine").Add(mineVersion).Add(List(mine)).Add(mineSwaps).Add(mineRounds).Add(mineDeploys), $"this game's {mine.Count} package(s) (version {mineVersion})"));
            (sentHost, sentVersion) = (host, mineVersion);
        }
        if (host == 0) (sentHost, sentVersion) = (0, -1);
        if (lobbyHost != 0 && !hosts.Contains(lobbyHost)) (lobbyHost, lobbyHostVersion, hostLobbyIds, hostLobbySwaps, hostLobbyRounds, hostLobbyDeploys) = (0, -1, [], "-", "-", "-");
    }

    // hosting: the lobby's, again whenever a friend's or this game's change. Called with gate held.
    void SendLobby(IReadOnlyCollection<ulong> hosted, List<(ulong To, Frame Frame, string What)> sends)
    {
        foreach (var gone in friends.Keys.Where(f => !hosted.Contains(f)).ToList()) friends.Remove(gone);
        foreach (var gone in lobbySent.Keys.Where(f => !hosted.Contains(f)).ToList()) lobbySent.Remove(gone);
        var lobbyNow = new SortedSet<ulong>(mine.Concat(friends.Values.SelectMany(f => f.Ids)).Take(MaxLobby));
        var swapsNow = string.Join(',', new[] { (Peer: transport.LocalId, Swaps: mineSwaps) }.Concat(friends.Select(f => (Peer: f.Key, f.Value.Swaps)))
            .Where(p => p.Swaps != "-").OrderBy(p => p.Peer)
            .SelectMany(p => p.Swaps.Split(',').Select(s => p.Peer.ToString("x", System.Globalization.CultureInfo.InvariantCulture) + ":" + s)).Take(MaxLobby));
        if (swapsNow.Length == 0) swapsNow = "-";
        var roundsNow = ByPlayer(mineRounds, f => f.Rounds);
        var deploysNow = ByPlayer(mineDeploys, f => f.Deploys);
        if (!lobbyNow.SetEquals(lobbyIds) || swapsNow != lobbySwaps || roundsNow != lobbyRounds || deploysNow != lobbyDeploys)
            (lobbyIds, lobbySwaps, lobbyRounds, lobbyDeploys, lobbyVersion) = (lobbyNow, swapsNow, roundsNow, deploysNow, lobbyVersion + 1);
        foreach (var friend in hosted)
            if (!lobbySent.TryGetValue(friend, out var had) || had != lobbyVersion)
            {
                sends.Add((friend, new Frame("assets.lobby").Add(lobbyVersion).Add(List(lobbyIds)).Add(friends.TryGetValue(friend, out var own) ? own.Version : -1).Add(lobbySwaps).Add(lobbyRounds).Add(lobbyDeploys),
                    $"the lobby's {lobbyIds.Count} package(s) and projectile swaps (version {lobbyVersion})"));
                lobbySent[friend] = lobbyVersion;
            }
    }

    // A list by player ("<peer hex>=<entries>" comma separated), "-" none; players past what one pipe line holds are left
    // out. Called with gate held.
    string ByPlayer(string own, Func<(long Version, SortedSet<ulong> Ids, string Swaps, string Rounds, string Deploys), string> theirs)
    {
        var roundsNow = "";
        foreach (var (peer, list) in new[] { (Peer: transport.LocalId, List: own) }.Concat(friends.Select(f => (Peer: f.Key, List: theirs(f.Value))))
                     .Where(p => p.List != "-").OrderBy(p => p.Peer))
        {
            var group = peer.ToString("x", System.Globalization.CultureInfo.InvariantCulture) + "=" + list;
            if (roundsNow.Length + group.Length + 1 > MaxRoundsText) break;
            roundsNow += (roundsNow.Length > 0 ? "," : "") + group;
        }
        return roundsNow.Length == 0 ? "-" : roundsNow;
    }

    // Asks this game to load friends' packages, once per change (a refusal is tried again next second).
    async Task PreloadAsync(SortedSet<ulong> want)
    {
        var text = List(want);
        lock (gate) if (text == preloaded || preloaded == "" && want.Count == 0) return;
        if (!live.Connected || live.Mismatched || !await preloading.WaitAsync(0)) return;
        try
        {
            var result = await live.PreloadAsync(want);
            if (result.Ok) lock (gate) preloaded = text;
            log(result.Ok ? $"Assets: this game loads {want.Count} package(s) for friends' changes." : "Assets: the game refused friends' packages: " + result.Detail);
        }
        finally { preloading.Release(); }
    }

    // Tells this game whose projectile swaps its copies of the players' shots fire, once per change.
    async Task ShotsAsync(string rules)
    {
        lock (gate) if (rules == shotsSent || shotsSent == "" && rules == "-") return;
        if (!live.Connected || live.Mismatched || !await preloading.WaitAsync(0)) return;
        try
        {
            var result = await live.ShotsAsync(rules);
            if (result.Ok) lock (gate) shotsSent = rules;
            log(result.Ok ? $"Shots: this game fires {(rules == "-" ? 0 : rules.Split(',').Length)} player projectile swap(s)."
                : "Shots: the game refused the players' projectile swaps: " + result.Detail);
        }
        finally { preloading.Release(); }
    }

    // Tells this game whose round changes its copies of the players' shots fly with, once per change. A game whose mod
    // loader predates them refuses the request: not asked again until the list changes.
    async Task RoundsAsync(string list)
    {
        lock (gate) if (list == roundsSent || roundsSent == "" && list == "-") return;
        if (!live.Connected || live.Mismatched || !await preloading.WaitAsync(0)) return;
        try
        {
            var result = await live.RoundsAsync(list);
            lock (gate) roundsSent = list;
            log(result.Ok ? $"Rounds: this game's copies of {(list == "-" ? 0 : list.Split(',').Length)} player(s)' shots fly with their own round changes."
                : "Rounds: the game refused the players' round changes: " + result.Detail);
        }
        finally { preloading.Release(); }
    }

    // Tells this game whose values its copies of the players' deployables carry, once per change. A game whose mod loader
    // predates them refuses the request: not asked again until the list changes.
    async Task DeploysAsync(string list)
    {
        lock (gate) if (list == deploysSent || deploysSent == "" && list == "-") return;
        if (!live.Connected || live.Mismatched || !await preloading.WaitAsync(0)) return;
        try
        {
            var result = await live.DeploysAsync(list);
            lock (gate) deploysSent = list;
            log(result.Ok ? $"Deploys: this game's copies of {(list == "-" ? 0 : list.Split(',').Length)} player(s)' deployables carry their own values."
                : "Deploys: the game refused the players' deployable changes: " + result.Detail);
        }
        finally { preloading.Release(); }
    }

    public async ValueTask DisposeAsync()
    {
        live.Received -= FromGame;
        live.StateChanged -= stateChanged;
        await timer.DisposeAsync();
        preloading.Dispose();
    }
}
