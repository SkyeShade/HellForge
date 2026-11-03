using System.Diagnostics;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// Changes the running session's mods without a restart (HellForge-Open docs/mods.md "Changing mods while you play"): the
// Mods page's changes (the launcher asks: GET and POST /hellforge/live-mods) or, after a mod sync, the host's set. It plans
// the change (LiveModPlan; a mod built on premium content the player does not own waits for a restart: Premium), sends it
// to the game's loader over the live pipe (native/mod-loader/live_mods.c) and waits for the loader's result. A change with
// source mods or game data is prepared first by HellForge.Cli mods-live (the backend cannot build them itself): the
// builds' DLLs and the change's hot package (LivePrepared). Then the session runs the new mods: its session-mods.json and
// modset.json say so, and Changed gives the new set to the lobby, mod sync and the directory, so joins are checked against
// the new digest. From the change's first line until the game's result the session's mods are not known (Changing): joins
// wait rather than be checked against a digest the game may no longer play. A result that comes after the wait for it
// still settles the session (the files, Changed), so the backend's view and digest follow what the game ends up running.
public sealed class LiveMods
{
    public sealed record Result(bool Live, bool Applied, IReadOnlyList<string> Changes, IReadOnlyList<string> Blocking, string Detail, string Digest);

    public static readonly TimeSpan ResultTimeout = TimeSpan.FromSeconds(30);
    // Building source mods and packaging game data for a change may take a while (a cold build compiles them).
    public static readonly TimeSpan PrepareTimeout = TimeSpan.FromMinutes(5);
    // The loader waits up to 90 s for a hot package to load (live_data.c), then reports.
    public static readonly TimeSpan DataResultTimeout = TimeSpan.FromSeconds(120);
    readonly LiveEditServer live;
    readonly string modsFolder, sessionFile, modSetFile;
    readonly string? cli;
    readonly Action<string> log;
    readonly SemaphoreSlim one = new(1, 1);
    readonly object gate = new();
    readonly Dictionary<string, TaskCompletionSource<string[]>> waiting = new(StringComparer.Ordinal);
    int changes;

    // cli: HellForge.Cli.exe, for changes that need builds or game data (null: those wait for the next session).
    public LiveMods(LiveEditServer live, string modsFolder, string sessionFile, string modSetFile, Action<string> log, string? cli = null)
    {
        this.live = live;
        this.cli = cli;
        this.modsFolder = modsFolder;
        this.sessionFile = sessionFile;
        this.modSetFile = modSetFile;
        this.log = log;
        live.Received += Heard;
    }

    // How long a change waits for the game's result before it answers that the game has not finished (the result still
    // settles the session when it comes); a change with game data waits DataResultTimeout.
    public TimeSpan ResultWait { get; init; } = ResultTimeout;

    // What this player owns of Arrowhead's premium content, by the rule the session's start used (PremiumMods.ForLiveChange:
    // the profile's sealed snapshot for the Steam account). A mod the change brings that builds on content not owned waits
    // for a restart, which loads it with its items locked (LiveModPlan.WithPremium). Unset: nothing is owned.
    public Func<PremiumContent>? Premium { get; init; }

    // The mod set the session plays with after a change.
    public event Action<ModSet>? Changed;

    // true: a change is being made, so the session's mods are not known until it settles; false: it settled without
    // changing them (Changed when it changed them).
    public event Action<bool>? Changing;

    void Heard(string[] parts)
    {
        if (parts.Length >= 6 && parts[0] == "modstate") log($"Live mods {parts[1]}: {parts[2]} {parts[3]}, {parts[4]} stats edit(s) applied, {parts[5]} refused.");
        if (parts.Length < 3 || parts[0] != "modresult") return;
        TaskCompletionSource<string[]>? waiter;
        lock (gate) waiting.Remove(parts[1], out waiter);
        waiter?.TrySetResult(parts);
    }

    public SessionMods Current() => SessionMods.Read(sessionFile);

    // The installation's mods as its Mods page has them, in load order.
    public List<ModManifest> Installation() => ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(modsFolder).Mods));

    // The change from the session's mods to `target`, a mod built on premium content the player does not own held back.
    LiveModPlan Plan(SessionMods current, IReadOnlyList<ModManifest> target) =>
        LiveModPlan.Make(current, target).WithPremium(Premium ?? (() => PremiumContent.Nothing));

    // What a change to `target` (the Mods page's mods when null) would do, without making it.
    public Result Preview(IReadOnlyList<ModManifest>? target = null)
    {
        var current = Current();
        var plan = Plan(current, target ?? Installation());
        var mine = current.SyncedWith.Length > 0 && target == null ? SyncedRefusal(current) : null;
        return new(plan.Live && mine == null, false, Describe(plan), mine != null ? [mine] : plan.Blocking,
            plan.Empty ? "Nothing changed." : plan.Live && mine == null ? "Applies now." : "Applies to the next session.", "");
    }

    // Makes the change. target: the Mods page's mods when null; sync: a host's set after a mod sync (syncedWith: the host).
    // expectDigest: the digest the change must give (mod sync: the host's), else it is not made.
    public async Task<Result> ApplyAsync(IReadOnlyList<ModManifest>? target, bool sync, string syncedWith, CancellationToken cancel, string? expectDigest = null)
    {
        // one change at a time: a change the game has not reported yet holds it until it settles; another waits as long as
        // a change may take, then is answered busy
        if (!await one.WaitAsync(PrepareTimeout + DataResultTimeout, cancel)) return new(true, false, [], ["Another change is under way."], "Try again in a moment.", "");
        Change? change = null;
        try
        {
            var current = Current();
            if (!sync && current.SyncedWith.Length > 0) return new(false, false, [], [SyncedRefusal(current)], "Applies to the next session.", "");
            var plan = Plan(current, target ?? Installation());
            var described = Describe(plan);
            if (plan.Empty) return new(true, false, [], [], "Nothing changed.", "");
            if (!plan.Live) return new(false, false, described, plan.Blocking, "Applies to the next session.", "");
            if (!live.Connected) return new(true, false, described, [], "The game is not connected yet; try again once it has loaded.", "");
            // what runs: the target, but for mods that did not load at the start and are unchanged since
            var active = plan.Target.Where(m => !current.Skipped.Any(s => s.Id == m.Id && s.Hash == ModSets.ContentHash(m))).ToList();
            var set = ModSet.Of(active);
            if (expectDigest != null && set.Digest != expectDigest)
                return new(false, false, described, ["The mods here do not make the host's mod list."], "Applies to the next session.", "");
            var txn = "live" + Interlocked.Increment(ref changes).ToString(System.Globalization.CultureInfo.InvariantCulture);
            var (prepared, problem) = await PrepareAsync(plan, txn, set.Digest, cancel);
            if (problem != null) return new(false, false, described, [problem], "Applies to the next session.", "");
            change = new Change(txn, plan, active, set, sync ? syncedWith : current.SyncedWith, described);
            return await CommitAsync(change, sync, prepared, cancel);
        }
        finally
        {
            if (change?.Settling != true) one.Release(); /* else the change's settling releases it */
        }
    }

    // A change for the game, and the session's view after it. Settling: it went to the game, whose result settles it
    // (SettleAsync), which then lets the next change begin.
    sealed record Change(string Txn, LiveModPlan Plan, List<ModManifest> Active, ModSet Set, string SyncedWith, List<string> Described)
    {
        public bool Settling { get; set; }
    }

    // The change sent to the game, then its result awaited for ResultWait: what it did, or that it has not finished yet
    // (the session follows it when it does). Joins wait meanwhile (Changing).
    async Task<Result> CommitAsync(Change change, bool sync, LivePrepared? prepared, CancellationToken cancel)
    {
        var waiter = new TaskCompletionSource<string[]>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (gate) waiting[change.Txn] = waiter;
        Changing?.Invoke(true);
        Result? refused;
        try { refused = await SendAsync(change.Plan, change.Txn, sync, change.Set.Digest, prepared); }
        catch (Exception)
        {
            lock (gate) waiting.Remove(change.Txn);
            Changing?.Invoke(false);
            throw;
        }
        if (refused != null)
        {
            lock (gate) waiting.Remove(change.Txn);
            Changing?.Invoke(false);
            return refused with { Changes = change.Described };
        }
        change.Settling = true;
        var settled = SettleAsync(change, waiter.Task);
        var timeout = prepared?.Data.Count > 0 ? DataResultTimeout : ResultWait;
        try { return await settled.WaitAsync(timeout, cancel); }
        catch (Exception error) when (error is TimeoutException or OperationCanceledException)
        {
            log(error is TimeoutException ? $"Live mods {change.Txn}: the game did not report its result within {timeout.TotalSeconds} s; the session follows it when it does."
                : $"Live mods {change.Txn}: the request was withdrawn; the session follows the game's result when it comes.");
            return new(true, false, change.Described, ["The game has not finished the change yet."],
                "This session follows it once the game reports it; until then friends cannot join.", "");
        }
    }

    // The game's result for a change, whenever it comes: a change it made (ok, or failed: made, but something of a mod
    // taken out may be left) is the session's from then on, its files written and its set given to Changed; one it refused
    // leaves the session as it was. Then the next change may begin.
    async Task<Result> SettleAsync(Change change, Task<string[]> result)
    {
        try
        {
            var done = await result;
            if (done[2] is not ("ok" or "failed"))
            {
                Changing?.Invoke(false);
                return new(false, false, change.Described, [Explain(string.Join('\t', done[3..]))], "Applies to the next session.", "");
            }
            SessionMods.Of(change.Plan.Target, change.Active, change.SyncedWith).Write(sessionFile);
            change.Set.Write(modSetFile);
            log($"Live mods {change.Txn}: {string.Join("; ", change.Described)}. The mods' digest is now {change.Set.Digest}." +
                (done[2] == "failed" ? " Something of a mod taken out may be left." : ""));
            Changed?.Invoke(change.Set);
            return done[2] == "ok" ? new(true, true, change.Described, [], "Applied.", change.Set.Digest)
                : new(true, false, change.Described, ["Something of a mod taken out may be left."], "Restart the session to be sure of its mods.", change.Set.Digest);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            log($"Live mods {change.Txn}: the session's mods could not be recorded: {error.Message}");
            return new(true, false, change.Described, ["The session's mods could not be recorded."], "Restart the session to be sure of its mods.", "");
        }
        finally { one.Release(); }
    }

    // The change sent to the loader: null when it took every line (its result comes later: Heard), else the refusal (its
    // Changes filled in by the caller).
    async Task<Result?> SendAsync(LiveModPlan plan, string txn, bool sync, string digest, LivePrepared? prepared)
    {
        foreach (var (verb, fields) in plan.Lines(txn, sync, digest, prepared))
        {
            var reply = await live.ModAsync(verb, fields);
            if (reply.Ok) continue;
            if (verb != "modbegin") await live.ModAsync("modabort", [txn]);
            log($"Live mods {txn} refused at {verb}: {reply.Detail.Replace('\t', ' ')}");
            return new(false, false, [], [Explain(reply.Detail)], "Applies to the next session.", "");
        }
        return null;
    }

    // What HellForge.Cli mods-live made for a change that needs it (source builds, the hot package): null with no problem
    // when the change needs nothing, else the prepared change or why it cannot be made now.
    async Task<(LivePrepared? Prepared, string? Problem)> PrepareAsync(LiveModPlan plan, string txn, string digest, CancellationToken cancel)
    {
        if (!plan.NeedsPreparation) return (null, null);
        if (cli == null || !File.Exists(cli)) return (null, "This session cannot build mods or game data while it runs.");
        var target = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(sessionFile))!, txn + "-target.txt");
        File.WriteAllLines(target, plan.Target.Select(m => m.Folder));
        var start = new ProcessStartInfo(cli) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[] { "mods-live", "--install", Path.GetDirectoryName(Path.GetFullPath(modsFolder))!, "--session-mods", sessionFile,
            "--target", target, "--seed", txn + ":" + digest + ":" + DateTime.UtcNow.Ticks.ToString(System.Globalization.CultureInfo.InvariantCulture) })
            start.ArgumentList.Add(argument);
        string output;
        try
        {
            using var process = Process.Start(start) ?? throw new InvalidOperationException("mods-live did not start");
            var errors = process.StandardError.ReadToEndAsync(cancel);
            output = await process.StandardOutput.ReadToEndAsync(cancel);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            timeout.CancelAfter(PrepareTimeout);
            await process.WaitForExitAsync(timeout.Token);
            foreach (var line in (await errors).Split('\n', StringSplitOptions.RemoveEmptyEntries)) log($"Live mods {txn} (mods-live): {line.TrimEnd()}");
            if (process.ExitCode != 0) return (null, "The change's mods could not be built or packaged (live-mods.log).");
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or IOException or OperationCanceledException)
        {
            log($"Live mods {txn}: mods-live failed: {error.Message}");
            return (null, "The change's mods could not be built or packaged.");
        }
        return Prepared(output, txn);
    }

    // mods-live's printed result: {"builds": {id: dll}, "data": [[verb, owner, ...]], "problems": [...]}.
    (LivePrepared? Prepared, string? Problem) Prepared(string output, string txn)
    {
        JsonObject? root;
        try { root = JsonNode.Parse(output) as JsonObject; }
        catch (System.Text.Json.JsonException) { root = null; }
        if (root == null) return (null, "The change's mods could not be prepared.");
        var problems = (root["problems"] as JsonArray ?? []).Select(p => (string?)p).OfType<string>().ToList();
        foreach (var problem in problems) log($"Live mods {txn}: {problem}");
        if (problems.Count > 0) return (null, problems[0]);
        var builds = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var (id, dll) in root["builds"] as JsonObject ?? []) if ((string?)dll is { } path) builds[id] = path;
        var data = (root["data"] as JsonArray ?? []).Select(l => (l as JsonArray ?? []).Select(f => (string?)f ?? "").ToArray()).Where(l => l.Length >= 3).ToList();
        if ((string?)root["package"] is { } package) log($"Live mods {txn}: hot package {package}, {data.Count - 1} override(s).");
        return (new LivePrepared(builds, data), null);
    }

    // A host's mods after a mod sync, in load order with this player's cosmetic mods and Local Stats (ModSyncSession.Resolve);
    // null when some are not here as the host has them.
    List<ModManifest>? HostOrder(ModSyncSession sync)
    {
        var (catalog, _) = ModCatalog.Load(modsFolder);
        return sync.Resolve(modsFolder, catalog).LoadOrder;
    }

    // Whether the running game can take a host's mods (mod sync's APPLY AND JOIN). A 0.3 host's digest counts its bundled
    // mods' files, which this machine cannot rebuild: its mods were found one by one by their hashes (Resolve) instead.
    public bool CanApplySync(ModSyncSession sync)
    {
        if (HostOrder(sync) is not { } order) return false;
        var current = Current();
        var plan = Plan(current, order);
        if (!plan.Live && !plan.Empty) return false;
        var active = plan.Target.Where(m => !current.Skipped.Any(s => s.Id == m.Id && s.Hash == ModSets.ContentHash(m))).ToList();
        return sync.Legacy || ModSet.Of(active).Digest == sync.Digest;
    }

    // The running game takes a host's mods: null when done, else why not.
    public async Task<string?> ApplySyncAsync(ModSyncSession sync, CancellationToken cancel)
    {
        if (HostOrder(sync) is not { } order) return "some of the host's mods are not here as the host has them";
        var result = await ApplyAsync(order, sync: true, sync.HostName.Length > 0 ? sync.HostName : sync.Host, cancel, expectDigest: sync.Legacy ? null : sync.Digest);
        if (result.Applied || result.Live && result.Changes.Count == 0) return null;
        return string.Join(" ", result.Blocking.DefaultIfEmpty(result.Detail));
    }

    static string SyncedRefusal(SessionMods current) =>
        $"This session plays {current.SyncedWith}'s mods (mod sync): your Mods page applies to your next session.";

    static List<string> Describe(LiveModPlan plan) => [.. plan.Changes.Select(c => c.Name + c.Action switch
    {
        LiveModAction.Add => " on",
        LiveModAction.Remove => " off",
        _ => " changed"
    })];

    // A loader refusal ("<reason>\t<why>") said for the player.
    static string Explain(string detail)
    {
        var parts = detail.Split('\t', 2);
        var why = parts.Length > 1 ? parts[1] : parts[0];
        return parts[0] switch
        {
            "coop" => "Friends are in the game: " + why + ".",
            "busy" => "Another change is under way.",
            "state" => "The game is still loading: " + why + ".",
            _ => why.Length > 0 ? char.ToUpperInvariant(why[0]) + why[1..] + "." : "The game refused the change."
        };
    }
}

// The session's LiveMods, once the live pipe exists (it is made after the backend's HTTP server starts).
public sealed class LiveModsHub
{
    public LiveMods? Service { get; set; }
}
