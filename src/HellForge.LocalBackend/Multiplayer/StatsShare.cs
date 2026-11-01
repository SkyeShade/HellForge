using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// Stats in co-op (docs/multiplayer.md "Stats in co-op", HellForge.Core.StatsSync). By default every player keeps their own
// (AssetShare loads the packages friends' changes need); a host may share its stats instead (the Mods page:
// ModSettings.HostStats), and then everyone in its lobby plays with them.
//   host:   goes to every friend in a lobby hosted here when they join, and again, numbered, whenever it changes:
//           "stats.set" [version, gzip of this machine's Local Stats file (mods/zz.local.stats/hellforge-stats.json, which
//           the in-game editor saves on APPLY; "" none), gzip of the in-game editor's mod content values
//           (hellforge-editor/mod-stats.json: content mods added, which Local Stats does not hold), gzip of its arm swaps
//           (the "mounts" of hellforge-editor/editor-settings.json: a vehicle's mount slot and the weapon it holds),
//           "host" (friends play with these) or "own" (they keep their own; the three files are sent empty)].
//   joiner: "stats.set" arrives from the friend whose lobby this machine joined. With "own" its game keeps its own values.
//           With "host" this backend serves the session's live-edit pipe (\\.\pipe\hellforge-live-<run id>,
//           native/mod-loader/live.c) and turns the host's file into live edits of the Local Stats mod in this game. The
//           host's mod content values go to host-mod-stats.json in the in-game editor's folder, which the editor applies in
//           place of its own while joined, and the host's Local Stats file to host-stats.json beside it, which the editor
//           shows as the saved changes (marks, its Changes list), read-only, and the host's arm swaps to host-mounts.json,
//           which the editor applies in place of its own. When the host goes (left, closed or silent) or stops sharing,
//           the files go and its own values come back.
// The game is told its role ("session" solo / host / joiner, whose stats it plays with, and the host's name): the in-game
// editor turns read-only while it plays with the host's. Fields live editing does not reach (attachments, status
// multipliers) stay each machine's own.
public sealed class StatsShare : IAsyncDisposable
{
    public const string FileName = "hellforge-stats.json";
    public const string ModValuesName = "mod-stats.json", HostModValuesName = "host-mod-stats.json", HostStatsName = "host-stats.json";
    public const string EditorSettingsName = "editor-settings.json", HostMountsName = "host-mounts.json";
    // What a host's file may unpack to (RecordGuard.HostFileLimits).
    const int HostFileBytes = 1 << 20;
    readonly IPeerTransport transport;
    readonly LobbyService lobby;
    readonly string statsFile, modValuesFile, hostModValuesFile, hostStatsFile, editorSettingsFile, hostMountsFile;
    // A sandboxed game's editor saves into the run's sandbox-stats folder (changes.json, editor-settings.json) until the
    // session ends (SandboxStats.Apply): null when the game is not sandboxed.
    readonly string? sandboxChangesFile, sandboxSettingsFile;
    readonly Func<ulong, string> nameOf;
    readonly Action<string> log;
    readonly LiveEditServer live;
    // The session's live-edit pipe, which mod sync shares (ModSync's offers to the game, its answers back).
    public LiveEditServer Live => live;

    // Whether joiners of this player's lobbies play with edited stats: it shares them (ModSettings.HostStats) and has
    // edits. A public lobby's listing says so (DirectoryService).
    public bool SharesEdited => hostShares() && Edited;

    // Whether this player plays with edited stats: Local Stats edits, the in-game editor's mod content values or arm swaps.
    public bool Edited
    {
        get
        {
            string? stats, mods, mounts;
            lock (gate) (stats, mods, mounts) = (mine, mineMods, mineMounts);
            return StatsSync.Values(stats, out var bootOnly).Count > 0 || bootOnly > 0 || !string.IsNullOrEmpty(mounts) && mounts != "{}" || HasModValues(mods);
        }
    }

    // mod-stats.json: {category: {target: {field: number}}}; any number is an edit.
    static bool HasModValues(string? json)
    {
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            return System.Text.Json.Nodes.JsonNode.Parse(json) is System.Text.Json.Nodes.JsonObject root && root.Select(c => c.Value).OfType<System.Text.Json.Nodes.JsonObject>()
                .SelectMany(c => c.Select(t => t.Value)).OfType<System.Text.Json.Nodes.JsonObject>().SelectMany(t => t.Select(f => f.Value)).OfType<System.Text.Json.Nodes.JsonValue>().Any();
        }
        catch (System.Text.Json.JsonException) { return false; }
    }
    readonly StatsSync sync = new();
    readonly SemaphoreSlim applying = new(1, 1);
    readonly object gate = new();
    // Notices go to the game one after another, in the order they were made (each continues the one before).
    readonly object noticesGate = new();
    Task notices = Task.CompletedTask;
    readonly Timer timer;
    // host: the file last read, its version, whether friends play with it, and the friends that have it
    string? mine, mineMods, mineSettings, mineMounts;
    DateTime mineStamp, modsStamp, settingsStamp;
    long length = -1, modsLength = -1, settingsLength = -1;
    // sandboxed: the files as they are, and the editor's saves of this session that go over them
    string? realStats, realMods, realSettings, sandboxChanges, sandboxSettings;
    DateTime changesStamp, sandboxSettingsStamp;
    long changesLength = -1, sandboxSettingsLength = -1;
    long version;
    bool sharing;
    readonly Func<bool> hostShares;
    readonly HashSet<ulong> sentTo = [];
    // joiner: the host this game is joined to, its last version, whether it shares its stats, and its files
    ulong hostPeer;
    long hostVersion = -1;
    bool hostShared;
    string? hostStats, hostMods, hostMounts;
    bool hostWritePending; /* under gate: the host's files on disk are not the latest (WriteHostNow) */
    readonly object hostWriteGate = new(); /* one WriteHostNow at a time; taken before gate */
    string role = "solo", roleHost = "";
    bool sessionSent;

    public StatsShare(IPeerTransport transport, LobbyService lobby, string runId, string modsFolder, Func<ulong, string> nameOf, Action<string> log,
        StatsShareOptions? options = null)
    {
        var (hostShares, sandboxStats) = options ?? new StatsShareOptions();
        this.transport = transport;
        this.lobby = lobby;
        this.nameOf = nameOf;
        this.log = log;
        this.hostShares = hostShares ?? (() => false);
        statsFile = Path.Combine(modsFolder, "zz.local.stats", FileName);
        if (sandboxStats != null)
        {
            sandboxChangesFile = Path.Combine(sandboxStats, SandboxStats.ChangesName);
            sandboxSettingsFile = Path.Combine(sandboxStats, SandboxStats.SettingsName);
        }
        var editor = Path.Combine(modsFolder, BundledMods.EditorFolder);
        modValuesFile = Path.Combine(editor, ModValuesName);
        hostModValuesFile = Path.Combine(editor, HostModValuesName);
        hostStatsFile = Path.Combine(editor, HostStatsName);
        editorSettingsFile = Path.Combine(editor, EditorSettingsName);
        hostMountsFile = Path.Combine(editor, HostMountsName);
        WriteHostNow(); /* a session cut short leaves its host's files behind */
        live = new LiveEditServer(runId);
        live.StateChanged += () => { lock (gate) sessionSent = false; _ = ApplyAsync(); };
        live.Start();
        timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    // Every second: the host's file (sent to friends who joined or when it changed), and whether the host is still there.
    void Tick()
    {
        try
        {
            ReadMine();
            ReadSharing();
            var friends = lobby.HostedFriends();
            SendToFriends(friends);
            bool hostGone;
            lock (gate)
            {
                hostGone = hostPeer != 0 && !lobby.HostsJoinedLobby(hostPeer);
                if (hostGone)
                {
                    log($"Stats: the host {LobbyService.EntityId(hostPeer)} is gone" + (hostShared ? "; this game's own values come back." : "."));
                    (hostPeer, hostVersion, hostShared, hostStats, hostMods, hostMounts, hostWritePending) = (0, -1, false, null, null, null, true);
                }
                var (wantRole, wantHost) = hostPeer != 0 ? ("joiner", nameOf(hostPeer)) : friends.Count > 0 ? ("host", "") : ("solo", "");
                if (wantRole != role || wantHost != roleHost) (role, roleHost, sessionSent) = (wantRole, wantHost, false);
            }
            if (HostWritePending()) WriteHostNow(); /* the host gone (its files go), or a write that failed */
            if (hostGone || !sessionSent) _ = ApplyAsync();
        }
        catch (Exception error) { log("Stats sharing failed: " + error.Message); }
    }

    // Whether friends play with this machine's stats (the Mods page), read again each second.
    void ReadSharing()
    {
        var shares = hostShares();
        lock (gate)
            if (shares != sharing)
            {
                (sharing, version, sessionSent) = (shares, version + 1, false); /* the game's editor shows which */
                sentTo.Clear();
                log(shares ? "Stats: friends in lobbies hosted here play with this machine's stats." : "Stats: every player keeps their own stats.");
            }
    }

    // This host's file, or that friends keep their own, to every friend who joined since it last changed.
    void SendToFriends(IReadOnlyCollection<ulong> friends)
    {
        List<ulong> send;
        byte[] packed, packedMods, packedMounts;
        long sending;
        bool shared;
        lock (gate)
        {
            sentTo.IntersectWith(friends);
            send = friends.Where(f => !sentTo.Contains(f)).ToList();
            sentTo.UnionWith(send);
            packed = StatsSync.Pack(sharing ? mine ?? "" : "");
            packedMods = StatsSync.Pack(sharing ? mineMods ?? "" : "");
            packedMounts = StatsSync.Pack(sharing ? mineMounts ?? "" : "");
            sending = version;
            shared = sharing;
        }
        foreach (var friend in send)
        {
            transport.Send(friend, new Frame("stats.set").Add(sending).Add(packed).Add(packedMods).Add(packedMounts).Add(shared ? "host" : "own").Encode(),
                reliable: true);
            log(shared ? $"Stats: sent this host's Local Stats, mod content values and arm swaps (version {sending}, {packed.Length} + {packedMods.Length} + {packedMounts.Length} bytes) to {LobbyService.EntityId(friend)}."
                : $"Stats: told {LobbyService.EntityId(friend)} to keep its own stats (version {sending}).");
        }
    }

    // The Local Stats file, the editor's mod content values and its arm swaps, re-read when they changed (a new version
    // for friends; the editor's settings change for its own reasons too, so only a change of its mounts counts).
    void ReadMine()
    {
        if (sandboxChangesFile != null) { ReadMineSandboxed(); return; }
        var changed = ReadChanged(statsFile, ref mineStamp, ref length, ref mine) | ReadChanged(modValuesFile, ref modsStamp, ref modsLength, ref mineMods);
        if (ReadChanged(editorSettingsFile, ref settingsStamp, ref settingsLength, ref mineSettings))
        {
            var mounts = MountsOf(mineSettings);
            lock (gate) if (mounts != mineMounts) { mineMounts = mounts; changed = true; }
        }
        if (changed)
            lock (gate)
            {
                version++;
                sentTo.Clear();
            }
    }

    // Sandboxed, the game's editor cannot write the mods folder: its saves wait in the run's sandbox-stats folder until the
    // session ends. What this host plays with meanwhile is its files with those saves over them (SandboxStats.Overlay,
    // nothing written), and that is what friends get, live, as they would from an unsandboxed host. The same text is
    // LocalStats, whose projectile swaps AssetShare sends.
    void ReadMineSandboxed()
    {
        var changed = ReadChanged(statsFile, ref mineStamp, ref length, ref realStats) | ReadChanged(modValuesFile, ref modsStamp, ref modsLength, ref realMods) |
            ReadChanged(sandboxChangesFile!, ref changesStamp, ref changesLength, ref sandboxChanges) |
            ReadChanged(editorSettingsFile, ref settingsStamp, ref settingsLength, ref realSettings) |
            ReadChanged(sandboxSettingsFile!, ref sandboxSettingsStamp, ref sandboxSettingsLength, ref sandboxSettings);
        if (!changed) return;
        string? real, mods, changes, settings;
        lock (gate) (real, mods, changes, settings) = (realStats, realMods, sandboxChanges, sandboxSettings ?? realSettings);
        var (stats, values) = SandboxStats.Overlay(real, mods, changes, EntityProfile.Current);
        var mounts = MountsOf(settings);
        lock (gate)
        {
            if (stats == mine && values == mineMods && mounts == mineMounts) return;
            (mine, mineMods, mineSettings, mineMounts) = (stats, values, settings, mounts);
            version++;
            sentTo.Clear();
        }
    }

    // The arm swaps of the editor's settings: its "mounts" object as JSON ("" none or unreadable).
    static string MountsOf(string? settings)
    {
        if (string.IsNullOrWhiteSpace(settings)) return "";
        try { return System.Text.Json.Nodes.JsonNode.Parse(settings) is System.Text.Json.Nodes.JsonObject root && root["mounts"] is System.Text.Json.Nodes.JsonObject mounts ? mounts.ToJsonString() : ""; }
        catch (System.Text.Json.JsonException) { return ""; }
    }

    // A file's text, when its time or size changed and its text with them; false when unchanged or being written.
    bool ReadChanged(string path, ref DateTime seenStamp, ref long seenLength, ref string? seen)
    {
        var info = new FileInfo(path);
        var stamp = info.Exists ? info.LastWriteTimeUtc : DateTime.MinValue;
        var size = info.Exists ? info.Length : -1;
        lock (gate) if (stamp == seenStamp && size == seenLength) return false;
        string? text = null;
        try { if (info.Exists) text = File.ReadAllText(path); }
        catch (IOException) { return false; } /* the editor is writing it: next second */
        lock (gate)
        {
            (seenStamp, seenLength) = (stamp, size);
            if (text == seen) return false;
            seen = text;
            return true;
        }
    }

    // The host's Local Stats file and mod content values for the in-game editor (null: none, the files go), each written
    // whole (the editor reads them each second and skips one caught mid-write).
    // Always the host's latest (hostStats, hostMods, hostMounts, read under the write lock), so two writes at once (two
    // versions from the transport) cannot leave the older on disk. A write that failed (the editor had the file open
    // as it was replaced) is made again on the next second's tick, not left until the host's next version.
    void WriteHostNow()
    {
        lock (hostWriteGate)
        {
            string? stats, mods, mounts;
            lock (gate) (stats, mods, mounts, hostWritePending) = (hostStats, hostMods, hostMounts, false);
            if (!WriteHost(stats, mods, mounts)) lock (gate) hostWritePending = true;
        }
    }

    bool HostWritePending() { lock (gate) return hostWritePending; }

    // True when all three were written (or removed).
    bool WriteHost(string? stats, string? mods, string? mounts)
    {
        var written = Write(hostStatsFile, stats, "Local Stats");
        written &= Write(hostModValuesFile, mods, "mod content values");
        written &= Write(hostMountsFile, mounts, "arm swaps");
        return written;
        bool Write(string path, string? text, string what)
        {
            try
            {
                // The in-game editor parses these natively: only a JSON object within RecordGuard's limits, written out
                // by System.Text.Json, reaches it.
                if (text is { Length: > 0 })
                {
                    text = Canonical(text, out var dropped);
                    if (text == null) log($"Stats: the host's {what} is not a file the editor reads; it is left out.");
                    else if (dropped > 0) log($"Stats: {dropped} value(s) of the host's {what} are beyond the limits and left out.");
                }
                if (text == null) { if (File.Exists(path)) File.Delete(path); return true; }
                if (!Directory.Exists(Path.GetDirectoryName(path))) return true;
                var partial = path + ".partial";
                File.WriteAllText(partial, text);
                File.Move(partial, path, overwrite: true);
                return true;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                log($"Stats: the host's {what} could not be written (tried again in a second): " + error.Message);
                return false;
            }
        }
    }

    static string? Canonical(string text, out int dropped)
    {
        dropped = 0;
        try { return System.Text.Json.Nodes.JsonNode.Parse(text) is System.Text.Json.Nodes.JsonObject root && RecordGuard.Bounded(root, RecordGuard.HostFileLimits, out dropped) is { } kept ? kept.ToJsonString() : null; }
        catch (System.Text.Json.JsonException) { return null; }
    }

    // A friend's backend: the host's stats, or that every player keeps their own, accepted only from the friend whose lobby
    // this machine joined.
    public void FromPeer(ulong peer, Frame frame)
    {
        if (frame.Type != "stats.set" || !lobby.HostsJoinedLobby(peer)) return;
        var shared = frame.Count <= 4 || frame.Text(4) != "own"; /* a host from before "own": it shares */
        string? text = null, mods = null, mounts = null;
        try
        {
            if (shared)
            {
                text = StatsSync.Unpack(frame.Bytes(1), HostFileBytes);
                mods = frame.Count > 2 ? StatsSync.Unpack(frame.Bytes(2), HostFileBytes) : ""; /* a host from before mod content values: none */
                mounts = frame.Count > 3 ? StatsSync.Unpack(frame.Bytes(3), HostFileBytes) : ""; /* a host from before arm swaps: none */
            }
        }
        catch (Exception error) when (error is InvalidDataException or IOException) { log("Stats: the host's stats could not be read: " + error.Message); return; }
        if (!long.TryParse(frame.Text(0), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var received)) return;
        lock (gate)
        {
            if (peer == hostPeer && received <= hostVersion) return;
            var name = nameOf(peer);
            if (role != "joiner" || roleHost != name || hostShared != shared) (role, roleHost, sessionSent) = ("joiner", name, false);
            (hostPeer, hostVersion, hostShared, hostStats, hostMods, hostMounts) = (peer, received, shared, text, mods, mounts);
        }
        log(shared ? $"Stats: the host's Local Stats, version {received}, from {LobbyService.EntityId(peer)}."
            : $"Stats: the host {LobbyService.EntityId(peer)} lets every player keep their own stats (version {received}).");
        WriteHostNow();
        _ = ApplyAsync();
    }

    // A message for the player, which the in-game editor shows over the game (a refused join: MultiplayerHost); kind
    // "error", "info" or "brief" (a note in the corner). Not kept: a game that is not connected does not get it.
    public void Notice(string kind, string title, IReadOnlyList<string> lines)
    {
        if (!live.Connected || live.Mismatched) { log($"Notice not shown, the game is not connected: {title}"); return; }
        // Off the caller's thread (it may hold the lobby service's lock), as a Task.Run each would be, but never overtaking
        // an earlier notice: the game gets them in the order they were made.
        lock (noticesGate) notices = notices.ContinueWith(_ => ShowNotice(kind, title, lines), CancellationToken.None,
            TaskContinuationOptions.None, TaskScheduler.Default).Unwrap();
    }

    async Task ShowNotice(string kind, string title, IReadOnlyList<string> lines)
    {
        var result = await live.NoticeAsync(kind, title, lines);
        log(result.Ok ? $"Notice shown in the game: {title}" : $"Notice not shown ({result.Detail}): {title}");
    }

    // Whether the lobby plays with its host's stats, as the game is told (HfApi stats_source): joined to a host who shares
    // them, or hosting and sharing them. Called with gate held.
    bool LobbyShared() => role == "joiner" ? hostPeer != 0 && hostShared : role == "host" && sharing;
    public bool LobbyPlaysHostStats { get { lock (gate) return LobbyShared(); } }
    // This machine's Local Stats file as last read (AssetShare: this player's projectile swaps).
    public string? LocalStats { get { lock (gate) return mine; } }

    // Takes the game to the host's values (joined to a host who shares them) or its own, one request at a time; a request
    // the game refused is logged and tried again with the next change.
    async Task ApplyAsync()
    {
        if (!live.Connected || live.Mismatched) return;
        await applying.WaitAsync();
        try
        {
            // this game's own files as they are now, not as the last tick read them: a join (the host's values) can come
            // before the first tick, and its own changes would then not give way
            ReadMine();
            string? host, own;
            string sessionRole, sessionHost;
            bool sessionShared;
            lock (gate)
                (host, own, sessionRole, sessionHost, sessionShared) = (hostPeer != 0 && hostShared ? hostStats ?? "" : null, mine, role, roleHost, LobbyShared());
            await SendSessionAsync(sessionRole, sessionHost, sessionShared);
            var requests = sync.Plan(host, own);
            var (done, refused) = await RequestAllAsync(requests, host != null);
            if (requests.Count > 0)
                log($"Stats: {(host != null ? "the host's values" : "this game's own values")}: {done} applied, {refused} refused" +
                    (host != null && sync.BootOnly > 0 ? $", {sync.BootOnly} apply only from a session start (attachments, status multipliers)" : "") + ".");
        }
        catch (Exception error) { log("Stats: applying failed: " + error.Message); }
        finally { applying.Release(); }
    }

    // The game's role, when it has not been told it since it changed (or the game connected).
    async Task SendSessionAsync(string sessionRole, string sessionHost, bool sessionShared)
    {
        bool sendSession;
        lock (gate) sendSession = !sessionSent;
        if (!sendSession) return;
        var session = await live.SessionAsync(sessionRole, sessionHost, sessionShared ? "host" : "own");
        lock (gate) if (session.Ok && sessionRole == role && sessionHost == roleHost && sessionShared == LobbyShared()) sessionSent = true;
    }

    // The plan's requests, one at a time (joined: to the host's values); how many the game took and refused, the first
    // five refusals logged.
    async Task<(int Done, int Refused)> RequestAllAsync(List<StatsSyncRequest> requests, bool joined)
    {
        int done = 0, refused = 0;
        foreach (var request in requests)
        {
            var result = request.Verb switch
            {
                "set" => await live.SetAsync(request.Field, request.Bits, request.Field.Shared),
                "base" => await live.BaseAsync(request.Field),
                _ => await live.ResetAsync(request.Field),
            };
            if (result.Ok) { sync.Done(request, joined); done++; }
            else if (refused++ < 5) log($"Stats: {request.Verb} {request.Field.Name} refused: {result.Detail}");
        }
        return (done, refused);
    }

    public async ValueTask DisposeAsync()
    {
        await timer.DisposeAsync();
        await live.DisposeAsync();
        applying.Dispose();
    }
}

// HostShares: whether friends play with this machine's stats when it hosts (ModSettings.HostStats; read each second).
// SandboxStats: the run's sandbox-stats folder when the game is sandboxed, else null.
public sealed record StatsShareOptions(Func<bool>? HostShares = null, string? SandboxStats = null);
