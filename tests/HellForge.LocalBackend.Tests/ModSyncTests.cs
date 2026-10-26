using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// Mod sync end to end: a host's and a joiner's lobby services and mod sync over the in-memory network, the joiner's game
// a display that records each offer. Every mod is synthetic.
static partial class ModSyncTests
{
    const ulong HostId = 0x110000100000021, GuestId = 0x110000100000022, StrangerId = 0x110000100000023;

    sealed class Display : ISyncDisplay
    {
        public readonly BlockingCollection<SyncView> Views = new();
        public Task<bool> ShowAsync(SyncView view, bool withMods) { Views.Add(view); return Task.FromResult(true); }

        // The next view in one of these states (progress views on the way are skipped).
        public SyncView Next(Action<bool, string> check, params string[] states)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
            while (Views.TryTake(out var view, deadline - DateTime.UtcNow > TimeSpan.Zero ? deadline - DateTime.UtcNow : TimeSpan.Zero))
                if (states.Contains(view.State)) return view;
                else if (view.State is not ("downloading" or "offer")) { check(false, $"expected {string.Join("/", states)}, got {view.State}: {view.Message}"); return view; }
            check(false, $"expected {string.Join("/", states)}, got nothing");
            return null!;
        }
    }

    sealed class Node
    {
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly LobbyService Lobby;
        public readonly ModSync Sync;
        public readonly Display Shown = new();
        public readonly string Mods, Run, SessionFile;
        public readonly List<string> Log = [];
        public readonly ulong Id;

        public Node(InMemoryNetwork network, ulong id, string folder, ModSet own, Func<string, string, bool> verified)
            : this(network.Join(id), folder, own, verified) { }

        public Node(IPeerTransport transport, string folder, ModSet own, Func<string, string, bool> verified)
        {
            Id = transport.LocalId;
            Mods = Path.Combine(folder, "mods");
            Run = Directory.CreateDirectory(Path.Combine(folder, "run")).FullName;
            var evidence = Directory.CreateDirectory(Path.Combine(Run, "backend")).FullName;
            SessionFile = ModSyncSession.PathIn(new InstallLayout(folder));
            void Note(string line) { lock (Log) Log.Add(line); }
            Lobby = new LobbyService(transport, Inbox.Add, Note, own.Digest);
            Sync = new ModSync(transport, new ModSyncPaths(Mods, Run, evidence, SessionFile), own, peer => "Player " + LobbyService.EntityId(peer), Note, verified) { Display = Shown };
            Lobby.ModsRefused = Sync.OfferMods;
            Sync.Expected = Lobby.JoinedRecently;
            transport.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("mods.", StringComparison.Ordinal)) Sync.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
        }

        public Frame Next(string type, Action<bool, string> check)
        {
            var got = Inbox.TryTake(out var frame, TimeSpan.FromSeconds(5));
            check(got && frame!.Type == type, $"expected {type}, got {(got ? frame!.Type : "nothing")}");
            return frame!;
        }
    }

    static void Write(string folder, string relative, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(folder, relative))!);
        File.WriteAllText(Path.Combine(folder, relative), text);
    }

    const string Ammo = """{"format":1,"id":"test.ammo","name":"Ammo","version":"1","native":{"networkFieldTypes":[{"name":"deposit_value","expect":{"kind":1,"bits":10,"min":0},"bits":14}]}}""";

    // A data patch of size bytes: an empty resource table, then a pattern.
    static void Patch(string path, int size, byte seed)
    {
        var bytes = new byte[size];
        for (var i = 72; i < size; i++) bytes[i] = (byte)(i * 31 + seed);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, bytes);
    }

    const string Stats = """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}}}""";

    // The host: network field types, schema stats (content), a 600 KB data patch (raw, several chunks) and a prebuilt DLL
    // (code). The joiner has the network field types only.
    static (ModSet Host, ModSet Guest) Fixture(string root)
    {
        var host = Path.Combine(root, "host", "mods");
        Write(Path.Combine(host, "ammo"), ModCatalog.ManifestName, Ammo);
        Write(Path.Combine(host, "texture"), ModCatalog.ManifestName, """{"format":1,"id":"test.texture","name":"Texture","version":"2"}""");
        Patch(Path.Combine(host, "texture", "data", "9ba626afa44a3aa3.patch_0"), 600_000, 7);
        Write(Path.Combine(host, "code"), ModCatalog.ManifestName, """{"format":1,"id":"test.code","name":"Code","version":"1","url":"https://example.com/code","code":{"dll":"mod.dll","sdk":1}}""");
        Write(Path.Combine(host, "code"), "mod.dll", "synthetic dll");
        Write(Path.Combine(host, "stats"), ModCatalog.ManifestName, """{"format":1,"id":"test.stats","name":"Stats","version":"1"}""");
        Write(Path.Combine(host, "stats"), StatsData.FileName, Stats);
        var guest = Path.Combine(root, "guest", "mods");
        Write(Path.Combine(guest, "ammo"), ModCatalog.ManifestName, Ammo);
        return (ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(host).Mods)), ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(guest).Mods)));
    }

    // No download folder of a mod is left (each attempt has its own: <id>@<hash>-<attempt>).
    static bool NoPartial(string mods, string id)
    {
        var folder = Path.Combine(SyncedStore.Root(mods), SyncedStore.PartialFolder);
        return !Directory.Exists(folder) || !Directory.GetDirectories(folder, id + "@*").Any();
    }

    // The host creates a lobby; the guest joins and is refused for its mods: the offer it is shown.
    static SyncView Refused(Node host, Node guest, Action<bool, string> check, string op, params string[] states)
    {
        host.Lobby.FromLocal(MultiplayerTests.Create(op + "-create", host.Id));
        var created = host.Next("lobby.result", check);
        guest.Lobby.FromLocal(MultiplayerTests.Join(op + "-join", GuestId, created.Text(4)));
        var refused = guest.Next("lobby.result", check);
        check(refused.Text(2) == LobbyService.LobbyNotJoinable.ToString(), "The join is refused like a lobby that cannot be joined.");
        return guest.Shown.Next((ok, why) => { lock (guest.Log) lock (host.Log) check(ok, why + " | joiner log: " + string.Join(" / ", guest.Log.TakeLast(6)) +
            " | host log: " + string.Join(" / ", host.Log.TakeLast(6))); }, states);
    }

    public static async Task Run(string root, Action<bool, string> check)
    {
        root = Path.Combine(root, "mod-sync");
        var (hostSet, guestSet) = Fixture(root);
        var allowVerified = false;
        bool Verified(string id, string hash) => allowVerified && hostSet.Find(id, hash) is { } mod && ModSets.NeedsVerified(mod.Class);
        var network = new InMemoryNetwork();
        var host = new Node(network, HostId, Path.Combine(root, "host"), hostSet, Verified);
        var guest = new Node(network, GuestId, Path.Combine(root, "guest"), guestSet, Verified);
        SyncAction Action(SyncView view, string id) => view.Items.Single(i => i.Mod.Id == id).Action;

        // A friend's unverified code and raw data: the lobby cannot be joined until the player trusts them (or installs them),
        // and both are named; schema stats would download.
        var blocked = Refused(host, guest, check, "a", "blocked");
        check(Action(blocked, "test.ammo") == SyncAction.Have && Action(blocked, "test.stats") == SyncAction.Download &&
            Action(blocked, "test.texture") == SyncAction.Trust && Action(blocked, "test.code") == SyncAction.Trust &&
            blocked.Message.Contains("Code, Texture") && blocked.Message.Contains("TRUST"), "Unverified code and raw data: " + blocked.Message);
        check(File.Exists(Path.Combine(guest.Run, "backend", ModSync.OfferFile)), "The offer is in the session's logs.");

        // Verified code and raw data: an offer; accepted, everything downloads, is checked and stored; restart writes the
        // next session.
        allowVerified = true;
        var offer = Refused(host, guest, check, "b", "offer");
        var expected = hostSet.Mods.Where(m => m.Id != "test.ammo").Sum(m => m.Bytes);
        check(offer.Bytes == expected && Action(offer, "test.code") == SyncAction.Download && offer.Message.Contains("3 mod(s)"), "Offer: " + offer.Message);
        guest.Sync.Answer(["syncanswer", offer.Version.ToString(), "accept"]);
        var ready = guest.Shown.Next(check, "ready", "failed");
        check(ready.State == "ready" && ready.Done == expected, $"Downloaded: {ready.State} {ready.Message} ({ready.Done} of {expected})");
        foreach (var mod in hostSet.Mods.Where(m => m.Id != "test.ammo"))
        {
            var stored = SyncedStore.Find(guest.Mods, mod.Id, mod.Hash);
            check(stored != null && ModSets.ContentHash(ModCatalog.Parse(stored, EntityProfile.Current)) == mod.Hash, mod.Id + " is stored as the host has it.");
        }
        guest.Sync.Answer(["syncanswer", ready.Version.ToString(), "restart"]);
        var signal = Path.Combine(guest.Run, ModSync.StopSignal);
        var waited = Stopwatch.StartNew();
        while (!File.Exists(signal) && waited.Elapsed < TimeSpan.FromSeconds(10)) await Task.Delay(50);
        check(File.Exists(signal) && File.ReadAllText(signal) == ModSync.StopReason, "The session is asked to stop for a restart.");
        check(guest.Shown.Next(check, "ready").Message.Contains("Restarting"), "The game is told it restarts.");
        var session = ModSyncSession.Read(guest.SessionFile);
        var (order, problem) = session.Resolve(guest.Mods, ModCatalog.Load(guest.Mods).Mods);
        check(order != null && ModSet.Of(order).Digest == hostSet.Digest && session.Connection.StartsWith(LobbyService.ConnectionPrefix, StringComparison.Ordinal),
            "The next session plays the host's mods: " + problem);
        File.Delete(signal);

        // Joined again with everything here: nothing to download, a restart is enough. Answered from the launcher (a game
        // without the in-game editor): its file is taken within a second or two.
        var again = Refused(host, guest, check, "c", "offer");
        check(again.Bytes == 0 && again.Items.All(i => i.Action == SyncAction.Have) && again.Message.Contains("Restart"), "Ready: " + again.Message);
        var written = ModSyncOffer.Read(Path.Combine(guest.Run, "backend"));
        check(written != null && written.Version == again.Version && written.State == "offer" && written.Mods.Count == again.Items.Count &&
            written.Mods.All(m => m.Action == "have"), "The launcher reads the offer the backend wrote.");
        ModSyncOffer.Answer(Path.Combine(guest.Run, "backend"), again.Version, "decline");
        check(guest.Shown.Next(check, "none").Items.Count == 0 && !File.Exists(Path.Combine(guest.Run, "backend", ModSyncOffer.AnswerFileName)),
            "Declined from the launcher: the offer closes and the answer is taken.");

        // Blocking a mod from the offer: saved, and the lobby is shown as blocked.
        foreach (var mod in hostSet.Mods.Where(m => m.Id != "test.ammo")) Directory.Delete(SyncedStore.Find(guest.Mods, mod.Id, mod.Hash)!, recursive: true);
        var toBlock = Refused(host, guest, check, "d", "offer");
        guest.Sync.Answer(["syncanswer", toBlock.Version.ToString(), "block", "test.texture"]);
        var nowBlocked = guest.Shown.Next(check, "blocked");
        check(Action(nowBlocked, "test.texture") == SyncAction.Blocked && ModSettings.Read(guest.Mods).Blocked.Contains("test.texture") && nowBlocked.Message.Contains("Texture"),
            "Blocked: " + nowBlocked.Message);
        ModSettings.SetBlocked(guest.Mods, "test.texture", false);

        // A host that does not share: nothing downloads.
        ModSettings.SetShare(host.Mods, false);
        var unshared = Refused(host, guest, check, "e", "blocked");
        check(unshared.Message.Contains("does not share") && Action(unshared, "test.texture") == SyncAction.Manual, "Not shared: " + unshared.Message);
        ModSettings.SetShare(host.Mods, true);

        // The host serves only a granted machine, only the set's files and parts.
        var stranger = network.Join(StrangerId);
        var replies = new BlockingCollection<Frame>();
        stranger.Received += (_, bytes) => replies.Add(Frame.Decode(bytes));
        var texture = hostSet.Mods.Single(m => m.Id == "test.texture");
        Frame Ask(IPeerTransport from, Frame request)
        {
            from.Send(HostId, request.Encode(), reliable: true);
            check(replies.TryTake(out var reply, TimeSpan.FromSeconds(5)), "The host answers " + request.Type);
            return reply!;
        }
        check(Ask(stranger, new Frame("mods.files").Add("1").Add(texture.Id).Add(texture.Hash).Add(0)).Text(1) == "refused", "No grant, no files.");
        host.Lobby.FromLocal(MultiplayerTests.Create("f-create", HostId));
        var lobby = host.Next("lobby.result", check).Text(4);
        stranger.Send(HostId, MultiplayerTests.Join("f-join", StrangerId, lobby).Add(new string('0', 32)).Add(LobbyService.Protocol).Encode(), reliable: true);
        // The offer, the refusal and the join's result (the test transport may reorder them).
        var answers = new List<Frame>();
        while (answers.Count < 3 && replies.TryTake(out var answer, TimeSpan.FromSeconds(5))) answers.Add(answer);
        check(answers.Any(a => a.Type == "mods.offer" && a.Text(0) == lobby.Split(';')[1] && a.Text(2) == "1" && ModSet.Parse(a.Text(1), local: false).Digest == hostSet.Digest) &&
            answers.Any(a => a.Type == "lobby.refused" && Enumerable.Range(0, a.Count).Any(i => a.Text(i).Contains("MOD SYNC"))) &&
            answers.Any(a => a.Type == "lobby.result"), "A refusal for mods names its lobby and carries the set.");
        foreach (var bad in new[] {
                     new Frame("mods.get").Add("2").Add(texture.Id).Add(texture.Hash).Add("hellforge-mod.json").Add(0).Add(ModSync.ChunkBytes + 1),
                     new Frame("mods.get").Add("3").Add(texture.Id).Add(texture.Hash).Add("../ammo/hellforge-mod.json").Add(0).Add(10),
                     new Frame("mods.get").Add("4").Add(texture.Id).Add(texture.Hash).Add("data/9ba626afa44a3aa3.patch_0").Add(599_990).Add(100),
                     new Frame("mods.files").Add("5").Add("test.ammo").Add(new string('a', 64)).Add(0) })
            check(Ask(stranger, bad).Text(1) == "refused", "Refused: " + bad.Text(3 < bad.Count ? 3 : 1));
        var listed = Ask(stranger, new Frame("mods.files").Add("6").Add(texture.Id).Add(texture.Hash).Add(0));
        check(listed.Text(1) == "ok" && listed.Int(2) == texture.Files.Count && listed.Int(3) == 0 && listed.Count == 4 + 3 * texture.Files.Count, "A granted machine gets the file list.");
        // Pacing: at most PeerRate to one machine (600 KB in five chunks: the last waits for the first four's bytes, less
        // the PaceBurst a pacing clock may lag; ModSyncSpeedTests times a whole download against it).
        var clock = Stopwatch.StartNew();
        for (var offset = 0; offset < 600_000; offset += ModSync.ChunkBytes)
            check(Ask(stranger, new Frame("mods.get").Add("p" + offset).Add(texture.Id).Add(texture.Hash).Add("data/9ba626afa44a3aa3.patch_0")
                .Add(offset).Add(Math.Min(ModSync.ChunkBytes, 600_000 - offset))).Text(1) == "ok", "A chunk is served.");
        var paced = TimeSpan.FromSeconds(4.0 * ModSync.ChunkBytes / ModSync.PeerRate) - ModSync.PaceBurst - TimeSpan.FromMilliseconds(2);
        check(clock.Elapsed >= paced, $"The host paced its upload ({clock.ElapsedMilliseconds} ms, at least {paced.TotalMilliseconds:0} ms).");
        await HostLimits(network, host, hostSet, stranger, replies, lobby, check);

        // A host whose copy changed (same size, other bytes): the download fails its check and nothing is stored.
        var patch = Path.Combine(host.Mods, "texture", "data", "9ba626afa44a3aa3.patch_0");
        Patch(patch, 600_000, 8);
        var changed = Refused(host, guest, check, "g", "offer");
        guest.Sync.Answer(["syncanswer", changed.Version.ToString(), "accept"]);
        var failed = guest.Shown.Next(check, "failed");
        check(failed.Message.Contains("not the file the host listed") && SyncedStore.Find(guest.Mods, texture.Id, texture.Hash) == null &&
            NoPartial(guest.Mods, texture.Id), "Tampered: " + failed.Message);
        Patch(patch, 600_000, 7);

        // A host that calls its code mod content: the joiner reads the manifest and stops before the DLL.
        allowVerified = false;
        var code = hostSet.Mods.Single(m => m.Id == "test.code");
        Directory.Delete(SyncedStore.Find(guest.Mods, code.Id, code.Hash)!, recursive: true);
        var liar = new Node(network, 0x110000100000024, Path.Combine(root, "liar"), hostSet with { Mods = [.. hostSet.Mods.Where(m => m.Id is "test.ammo" || m.Id == code.Id).Select(m => m.Id == code.Id ? m with { Class = ModClass.Content } : m)] },
            Verified);
        liar.Lobby.FromLocal(MultiplayerTests.Create("h-create", 0x110000100000024));
        guest.Lobby.FromLocal(MultiplayerTests.Join("h-join", GuestId, liar.Next("lobby.result", check).Text(4)));
        guest.Next("lobby.result", check);
        var lied = guest.Shown.Next(check, "offer");
        check(Action(lied, "test.code") == SyncAction.Download, "The joiner first believes the host's class.");
        guest.Sync.Answer(["syncanswer", lied.Version.ToString(), "accept"]);
        var caught = guest.Shown.Next(check, "failed");
        check(caught.Message.Contains("runs code") && SyncedStore.Find(guest.Mods, code.Id, code.Hash) == null, "A lying host is caught: " + caught.Message);
        await Liars(network, guest, hostSet, root, check);
        await Hostile(network, guest, root, check);
        await Trusting(network, guest, hostSet, root, check);

        await LiveDisplay(root, check);
        Window(check);
        await Speed(root, check);
        await liar.Sync.DisposeAsync();
        await guest.Sync.DisposeAsync();
        await host.Sync.DisposeAsync();
    }

    // Hosts that call raw data content (the class kept to the verified list): a data patch, which its file list shows; raw
    // stats lines dressed as schema stats, which its stats file shows; and componentWrites, which its manifest shows. Each
    // stops before its other bytes, and nothing is stored.
    static async Task Liars(InMemoryNetwork network, Node guest, ModSet hostSet, string root, Action<bool, string> check)
    {
        var folder = Path.Combine(root, "raw-liars", "mods");
        var texture = hostSet.Mods.Single(m => m.Id == "test.texture");
        if (SyncedStore.Find(guest.Mods, texture.Id, texture.Hash) is { } stored) Directory.Delete(stored, recursive: true);
        Write(Path.Combine(folder, "rawstats"), ModCatalog.ManifestName, """{"format":1,"id":"test.rawstats","name":"Raw stats","version":"1"}""");
        Write(Path.Combine(folder, "rawstats"), StatsData.FileName,
            """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}},"raw":[{"component":"DepositComponentData","resource":"0x1","offset":0,"type":"u32","value":10,"expect":5}]}""");
        Write(Path.Combine(folder, "rawstats"), "padding.txt", new string('p', 300_000));
        Write(Path.Combine(folder, "writes"), ModCatalog.ManifestName, """{"format":1,"id":"test.writes","name":"Writes","version":"1","native":{"componentWrites":[{"component":"DepositComponentData","resource":"0x056DE1C5E21E723E","fields":[{"name":"capacity","offset":0,"expect":1000,"value":10000}]}]}}""");
        Write(Path.Combine(folder, "writes"), "padding.txt", new string('w', 300_000));
        var raw = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Load(folder).Mods));
        check(raw.Mods.Count == 2 && raw.Mods.All(m => m.Class == ModClass.Raw), "Raw stats lines and componentWrites are raw: " + string.Join(", ", raw.Mods.Select(m => m.Id + " " + m.Class)));
        ulong next = 0x110000100000030;
        foreach (var (id, set, file) in new[] { (texture.Id, hostSet, "data/9ba626afa44a3aa3.patch_0"), ("test.rawstats", raw, "padding.txt"), ("test.writes", raw, "padding.txt") })
        {
            var target = set.Mods.Single(m => m.Id == id);
            var liar = new Node(network, ++next, Path.Combine(root, "raw-liar-" + id), new ModSet(set.Digest, [target with { Class = ModClass.Content }]), (_, _) => false);
            liar.Lobby.FromLocal(MultiplayerTests.Create(id + "-create", next));
            guest.Lobby.FromLocal(MultiplayerTests.Join(id + "-join", GuestId, liar.Next("lobby.result", check).Text(4)));
            guest.Next("lobby.result", check);
            var lied = guest.Shown.Next(check, "offer");
            check(lied.Items.Single(i => i.Mod.Id == id).Action == SyncAction.Download, id + ": the joiner first believes the host's class.");
            guest.Sync.Answer(["syncanswer", lied.Version.ToString(), "accept"]);
            var caught = guest.Shown.Next(check, "failed");
            check(caught.Message.Contains("carries data HellForge cannot check") && caught.Done < target.Files.Single(f => f.Path == file).Size &&
                SyncedStore.Find(guest.Mods, id, target.Hash) == null && NoPartial(guest.Mods, id), $"{id}, called content, is caught before its bytes: {caught.Message} ({caught.Done} bytes)");
            await liar.Sync.DisposeAsync();
        }
    }

    static Frame Ask(IPeerTransport from, BlockingCollection<Frame> replies, Frame request, Action<bool, string> check)
    {
        from.Send(HostId, request.Encode(), reliable: true);
        check(replies.TryTake(out var reply, TimeSpan.FromSeconds(5)), "The host answers " + request.Type);
        return reply!;
    }

    // A machine of the test's own (raw frames) whose join to the host's lobby is refused for its mods: it gets a grant.
    static (IPeerTransport Peer, BlockingCollection<Frame> Replies) Refusee(InMemoryNetwork network, ulong id, string lobby, string op)
    {
        var peer = network.Join(id);
        var replies = new BlockingCollection<Frame>();
        peer.Received += (_, bytes) => replies.Add(Frame.Decode(bytes));
        JoinAgain(peer, replies, id, lobby, op);
        return (peer, replies);
    }

    // The offer, the refusal and the join's result.
    static void JoinAgain(IPeerTransport peer, BlockingCollection<Frame> replies, ulong id, string lobby, string op)
    {
        peer.Send(HostId, MultiplayerTests.Join(op, id, lobby).Add(new string('0', 32)).Add(LobbyService.Protocol).Encode(), reliable: true);
        for (var i = 0; i < 3 && replies.TryTake(out _, TimeSpan.FromSeconds(5)); i++) { }
    }

    // The host's limits (the review of PR 17): a reply it cannot send does not stop its worker; a grant's bytes and time are
    // capped, and joining again extends neither; code and raw mods go only to Steam friends.
    static async Task HostLimits(InMemoryNetwork network, Node host, ModSet hostSet, IPeerTransport stranger, BlockingCollection<Frame> replies, string lobby,
        Action<bool, string> check)
    {
        var texture = hostSet.Mods.Single(m => m.Id == "test.texture");
        const string Patch = "data/9ba626afa44a3aa3.patch_0";
        Frame Chunk(string req, long offset) => new Frame("mods.get").Add(req).Add(texture.Id).Add(texture.Hash).Add(Patch).Add(offset).Add(ModSync.ChunkBytes);

        // A request id near a frame's size: its 128 KiB chunk cannot be sent. Logged, and the next request is served.
        stranger.Send(HostId, new Frame("mods.get").Add(new string('r', 950_000)).Add(texture.Id).Add(texture.Hash).Add(Patch).Add(0).Add(ModSync.ChunkBytes).Encode(), reliable: true);
        var after = Ask(stranger, replies, new Frame("mods.files").Add("after").Add(texture.Id).Add(texture.Hash).Add(0), check);
        lock (host.Log)
            check(after.Text(0) == "after" && after.Text(1) == "ok" && host.Log.Any(l => l.Contains("could not be answered")),
                "The host's worker goes on after a reply it could not send: " + string.Join(" / ", host.Log.TakeLast(3)));

        // The grant's budget: the set twice over and a little; then refused, and joining again does not refill it.
        long served = 0;
        Frame last = after;
        for (var i = 0; i < 40 && last.Text(1) == "ok"; i++)
        {
            last = Ask(stranger, replies, Chunk("budget" + i, 0), check);
            if (last.Text(1) == "ok") served += ModSync.ChunkBytes;
        }
        var budget = 2 * hostSet.Mods.Sum(m => m.Bytes) + ModSync.MaxWindow * ModSync.ChunkBytes;
        check(last.Text(1) == "refused" && last.Text(2).Contains("all this offer allows") && served < budget, $"The grant's bytes are capped ({served} of {budget}): {last.Text(2)}");
        JoinAgain(stranger, replies, StrangerId, lobby, "f2-join");
        check(Ask(stranger, replies, Chunk("budget-again", 0), check).Text(1) == "refused", "Joining again does not refill the grant.");

        // The grant's time: from the offer, never extended by downloading. (3 s: the second ask, 0.6 s in, must still be
        // within it on a slow runner.)
        host.Sync.GrantFor = TimeSpan.FromSeconds(3);
        var (timed, timedReplies) = Refusee(network, 0x110000100000026, lobby, "timed-join");
        var granted = Stopwatch.StartNew();
        check(Ask(timed, timedReplies, Chunk("t1", 0), check).Text(1) == "ok", "A fresh grant serves.");
        await Task.Delay(TimeSpan.FromSeconds(0.6));
        check(Ask(timed, timedReplies, Chunk("t2", 0), check).Text(1) == "ok", "Within its time, it still serves.");
        while (granted.Elapsed < TimeSpan.FromSeconds(3.2)) await Task.Delay(50);
        var expired = Ask(timed, timedReplies, Chunk("t3", 0), check);
        check(expired.Text(1) == "refused" && expired.Text(2).Contains("time is up"), "The grant ends at its time, downloads or not: " + expired.Text(2));
        host.Sync.GrantFor = ModSync.GrantTime;
        timed.Dispose();

        // Not a Steam friend: content is served, a code or raw mod's files are not. A stranger reaches mod sync only through a
        // Public lobby: Friends only (a lobby with no privacy value) refuses them before any offer.
        host.Lobby.FromLocal(PrivacyTests.Privacy("public", host.Lobby.HostedLobbies().Single(l => l.ConnectionString == lobby).Id, HostId, "0"));
        // (the update's result, after whatever the game was told meanwhile: members' joins, the lobby's own update)
        while (host.Inbox.TryTake(out var told, TimeSpan.FromSeconds(5)) && !(told.Type == "lobby.result" && told.Text(0) == "public")) { }
        const ulong NotFriend = 0x110000100000027;
        lock (network.Strangers) network.Strangers.Add(NotFriend);
        var (other, otherReplies) = Refusee(network, NotFriend, lobby, "stranger-join");
        var stats = hostSet.Mods.Single(m => m.Id == "test.stats");
        var code = hostSet.Mods.Single(m => m.Id == "test.code");
        foreach (var mod in new[] { texture, code })
        {
            var refused = Ask(other, otherReplies, new Frame("mods.files").Add("x" + mod.Id).Add(mod.Id).Add(mod.Hash).Add(0), check);
            check(refused.Text(1) == "refused" && refused.Text(2).Contains("Steam friends"), $"{mod.Id} is not served to a stranger: {refused.Text(2)}");
        }
        check(Ask(other, otherReplies, new Frame("mods.files").Add("s").Add(stats.Id).Add(stats.Hash).Add(0), check).Text(1) == "ok", "Content is served to a stranger.");
        lock (network.Strangers) network.Strangers.Remove(NotFriend);
        other.Dispose();
        // what the host's game was told of that lobby since (its update, the stranger's refused join): not the next test's
        while (host.Inbox.TryTake(out _, TimeSpan.FromMilliseconds(300))) { }
    }

    // A host whose set this test makes up: the mods given, each served from its folder as listed.
    static ModSetEntry Made(string id, string folder, ModClass kind, string? hash = null, long? bytes = null, params ModFile[] extra)
    {
        var files = Directory.GetFiles(folder, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(folder, f).Replace('\\', '/')).Order(StringComparer.Ordinal)
            .Select(p =>
            {
                var data = File.ReadAllBytes(Path.Combine(folder, p));
                return new ModFile(p, data.Length, Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(data)));
            }).Concat(extra).ToList();
        return new ModSetEntry(id, id, "1", hash ?? new string('d', 64), bytes ?? files.Sum(f => f.Size), kind, 0) { Folder = folder, Files = files };
    }

    // The guest joins a host made of these mods (called content) and is refused: the offer it is shown.
    static SyncView Offered(InMemoryNetwork network, Node guest, ulong id, string root, string op, Action<bool, string> check, params ModSetEntry[] mods)
    {
        var host = new Node(network, id, Path.Combine(root, "hostile-" + op), new ModSet(new string('c', 32), mods), (_, _) => false);
        host.Lobby.FromLocal(MultiplayerTests.Create(op + "-create", id));
        guest.Lobby.FromLocal(MultiplayerTests.Join(op + "-join", GuestId, host.Next("lobby.result", check).Text(4)));
        guest.Next("lobby.result", check);
        return guest.Shown.Next(check, "offer");
    }

    // Hosts whose bytes would have crashed or stalled the joiner's download (the review of PR 17): each ends in "failed"
    // with a message and leaves no download folder. Then a mod of 2000 files (two file list pages), first on a full disk.
    static async Task Hostile(InMemoryNetwork network, Node guest, string root, Action<bool, string> check)
    {
        var folder = Path.Combine(root, "hostile");
        ulong next = 0x110000100000040;
        void Fails(string op, string expect, Func<SyncView, bool>? also, params ModSetEntry[] mods)
        {
            var offer = Offered(network, guest, ++next, root, op, check, mods);
            guest.Sync.Answer(["syncanswer", offer.Version.ToString(), "accept"]);
            var failed = guest.Shown.Next(check, "failed");
            check(failed.Message.Contains(expect) && (also?.Invoke(failed) ?? true) && mods.All(m => NoPartial(guest.Mods, m.Id) && SyncedStore.Find(guest.Mods, m.Id, m.Hash) == null),
                $"{op}: {failed.Message} ({failed.Done} bytes)");
        }

        // Sizes near long.MaxValue (they overflowed the sum of the file list).
        Write(Path.Combine(folder, "huge"), ModCatalog.ManifestName, Ammo.Replace("test.ammo", "test.huge"));
        Fails("huge", "cannot be taken", null, Made("test.huge", Path.Combine(folder, "huge"), ModClass.Content, bytes: 1000,
            extra: [new ModFile("big.bin", long.MaxValue - 10, new string('A', 64)), new ModFile("big2.bin", 20, new string('B', 64))]));
        // A manifest of the wrong shape: "native": 1, and a network field type without "expect".
        Write(Path.Combine(folder, "native"), ModCatalog.ManifestName, """{"format":1,"id":"test.native","name":"N","version":"1","native":1}""");
        Fails("native", "native must be an object", null, Made("test.native", Path.Combine(folder, "native"), ModClass.Content));
        Write(Path.Combine(folder, "expect"), ModCatalog.ManifestName,
            """{"format":1,"id":"test.expect","name":"E","version":"1","native":{"networkFieldTypes":[{"name":"deposit_value","bits":14}]}}""");
        Fails("expect", "expect is required", null, Made("test.expect", Path.Combine(folder, "expect"), ModClass.Content));
        // Two manifests: the one the catalogue reads (hellforge-mod.json) is a code mod's, the legacy one a content mod's.
        // The joiner reads the same one first and stops before the DLL.
        var twin = Path.Combine(folder, "twin");
        Write(twin, ModCatalog.LegacyManifestName, Ammo.Replace("test.ammo", "test.twin"));
        Write(twin, ModCatalog.ManifestName, """{"format":1,"id":"test.twin","name":"Twin","version":"1","code":{"dll":"mod.dll","sdk":1}}""");
        Write(twin, "mod.dll", new string('d', 300_000));
        var twinMod = ModCatalog.Parse(twin, EntityProfile.Current);
        check(ModSets.ClassOf(twinMod) == ModClass.Code, "The twin is a code mod by the manifest the catalogue reads.");
        Fails("twin", "runs code", view => view.Done < 300_000, Made("test.twin", twin, ModClass.Content, ModSets.ContentHash(twinMod)) with { Version = twinMod.Version });

        // 2000 files: more than a frame's fields in one list (4 + 3 per file), so the host pages it. First a disk too full.
        var bulk = Path.Combine(folder, "bulk");
        Write(bulk, ModCatalog.ManifestName,
            """{"format":1,"id":"test.bulk","name":"Bulk","version":"1","native":{"networkFieldTypes":[{"name":"remaining","expect":{"kind":1,"bits":8,"min":0},"bits":9}]}}""");
        for (var i = 0; i < 1999; i++) Write(bulk, $"files/f{i:D4}.txt", i.ToString());
        var bulkSet = ModSet.Of(ModCatalog.Load(Path.GetDirectoryName(bulk)!).Mods.Where(m => m.Id == "test.bulk").ToList());
        var bulkMod = bulkSet.Mods.Single();
        check(bulkMod.Files.Count == 2000 && bulkMod.Class == ModClass.Content, $"The bulk mod counts {bulkMod.Files.Count} files.");
        var bulkHost = new Node(network, ++next, Path.Combine(root, "hostile-bulk"), bulkSet, (_, _) => false);
        bulkHost.Lobby.FromLocal(MultiplayerTests.Create("bulk-create", next));
        var bulkLobby = bulkHost.Next("lobby.result", check).Text(4);
        SyncView JoinBulk(string op)
        {
            guest.Lobby.FromLocal(MultiplayerTests.Join(op, GuestId, bulkLobby));
            guest.Next("lobby.result", check);
            return guest.Shown.Next(check, "offer");
        }
        guest.Sync.FreeSpace = _ => 1000;
        var full = JoinBulk("bulk-full");
        guest.Sync.Answer(["syncanswer", full.Version.ToString(), "accept"]);
        var noRoom = guest.Shown.Next(check, "failed");
        check(noRoom.Message.Contains("Not enough free disk space") && noRoom.Done == 0, "A full disk: " + noRoom.Message);
        guest.Sync.FreeSpace = _ => null;
        var offer = JoinBulk("bulk-join");
        guest.Sync.Answer(["syncanswer", offer.Version.ToString(), "accept"]);
        var ready = guest.Shown.Next(check, "ready", "failed");
        check(ready.State == "ready" && SyncedStore.Find(guest.Mods, bulkMod.Id, bulkMod.Hash) != null && NoPartial(guest.Mods, bulkMod.Id),
            $"A 2000-file mod downloads, page by page: {ready.State} {ready.Message}");
        await bulkHost.Sync.DisposeAsync();

        // More mods than the loader holds: the ones that need nothing are left out first, and the message says so.
        var many = Enumerable.Range(0, 600).Select(i => new SyncItem(new ModSetEntry($"m.{i:D3}", "M", "1", new string('e', 64), 1, ModClass.Content, i),
            i < 100 ? SyncAction.Download : SyncAction.Have, null)).ToList();
        var (shown, message) = LiveSyncDisplay.Shown(new SyncView(1, "offer", "Host", 100, 0, "Download them.", many));
        check(shown.Count == LiveSyncDisplay.MaxMods && shown.Count(i => i.Action == SyncAction.Download) == 100 && message.StartsWith("(88 more mods"),
            $"An offer of 600 mods: {shown.Count} listed, {message}");
    }

    // TRUST and GET IT: a Steam friend's code and raw mods, not verified, wait for the player's trust; GET IT opens a mod's
    // own address; a trust names one exact mod the plan offers for trust, and the last one starts the download, every byte
    // checked as before. A stranger's are never offered for trust (trusted or not), and untrusted, a mod waits again.
    static async Task Trusting(InMemoryNetwork network, Node guest, ModSet hostSet, string root, Action<bool, string> check)
    {
        // a friend of its own: the first host's grant to the guest is spent by now
        var host = new Node(network, 0x110000100000051, Path.Combine(root, "trust-host"), hostSet, (_, _) => false);
        var code = hostSet.Mods.Single(m => m.Id == "test.code");
        var texture = hostSet.Mods.Single(m => m.Id == "test.texture");
        foreach (var mod in new[] { code, texture })
            if (SyncedStore.Find(guest.Mods, mod.Id, mod.Hash) is { } stored) Directory.Delete(stored, recursive: true);
        SyncAction Action(SyncView view, string id) => view.Items.Single(i => i.Mod.Id == id).Action;
        string Trusted() => string.Join(", ", ModSettings.Read(guest.Mods).Trusted.Select(t => t.Id));

        var asked = Refused(host, guest, check, "trust-a", "blocked");
        check(Action(asked, "test.code") == SyncAction.Trust && Action(asked, "test.texture") == SyncAction.Trust &&
            asked.Items.Single(i => i.Mod.Id == "test.code").Mod.Url == "https://example.com/code", "Both wait for trust; the code mod's address came with it: " + asked.Message);
        // a trust for a mod the plan does not offer for trust (content that downloads anyway, one not in the set) is dropped
        guest.Sync.Answer(["syncanswer", asked.Version.ToString(), "trust", "test.stats"]);
        guest.Sync.Answer(["syncanswer", asked.Version.ToString(), "trust", "test.other"]);
        check(Trusted() == "", "Only a mod offered for trust is trusted: " + Trusted());

        // GET IT: the mod's address from the host's set, handed to the browser; a mod without one opens nothing
        var opened = new BlockingCollection<string>();
        guest.Sync.Browse = opened.Add;
        guest.Sync.Answer(["syncanswer", asked.Version.ToString(), "open", "test.stats"]);
        guest.Sync.Answer(["syncanswer", asked.Version.ToString(), "open", "test.code"]);
        check(opened.TryTake(out var page, TimeSpan.FromSeconds(5)) && page == "https://example.com/code" && !opened.TryTake(out _, TimeSpan.FromMilliseconds(300)),
            "GET IT opens the code mod's page alone: " + page);

        // A stranger hosting the same mods (a Public lobby): nothing to trust, the player installs them.
        const ulong StrangerHost = 0x110000100000050;
        lock (network.Strangers) network.Strangers.Add(StrangerHost);
        var far = new Node(network, StrangerHost, Path.Combine(root, "trust-stranger"), hostSet, (_, _) => false);
        far.Lobby.FromLocal(MultiplayerTests.Create("trust-s-create", StrangerHost));
        var farLobby = far.Next("lobby.result", check).Text(4);
        far.Lobby.FromLocal(PrivacyTests.Privacy("public", far.Lobby.HostedLobbies().Single(l => l.ConnectionString == farLobby).Id, StrangerHost, "0"));
        while (far.Inbox.TryTake(out var told, TimeSpan.FromSeconds(5)) && !(told.Type == "lobby.result" && told.Text(0) == "public")) { }
        guest.Lobby.FromLocal(MultiplayerTests.Join("trust-s-join", GuestId, farLobby));
        guest.Next("lobby.result", check);
        var strange = guest.Shown.Next(check, "blocked");
        check(Action(strange, "test.code") == SyncAction.Manual && Action(strange, "test.texture") == SyncAction.Manual && strange.Items.All(i => i.Action != SyncAction.Trust) &&
            strange.Message.Contains("Steam friends"), "A stranger's code and raw data are the player's to install: " + strange.Message);
        guest.Sync.Answer(["syncanswer", strange.Version.ToString(), "trust", "test.code"]);
        check(Trusted() == "", "A stranger's mod cannot be trusted from the prompt.");
        guest.Sync.Answer(["syncanswer", strange.Version.ToString(), "decline"]);
        guest.Shown.Next(check, "none");
        await far.Sync.DisposeAsync();
        lock (network.Strangers) network.Strangers.Remove(StrangerHost);

        // Trust the code mod: kept by its exact content, with who hosted it; the offer is made again, the texture still waits.
        var again = Refused(host, guest, check, "trust-b", "blocked");
        guest.Sync.Answer(["syncanswer", again.Version.ToString(), "trust", "test.code"]);
        var one = guest.Shown.Next(check, "blocked");
        var kept = ModSettings.Read(guest.Mods).Trusted;
        check(Action(one, "test.code") == SyncAction.Download && Action(one, "test.texture") == SyncAction.Trust && kept.Count == 1 && kept[0].Id == code.Id &&
            kept[0].Hash == code.Hash && kept[0].From.Contains(LobbyService.EntityId(host.Id)), $"One trusted, the other waits: {one.Message} ({Trusted()})");
        // Trust the texture: nothing else waits, so it downloads at once (TRUST AND DOWNLOAD), each file checked and stored.
        guest.Sync.Answer(["syncanswer", one.Version.ToString(), "trust", "test.texture"]);
        var ready = guest.Shown.Next(check, "ready", "failed");
        check(ready.State == "ready" && SyncedStore.Find(guest.Mods, code.Id, code.Hash) != null && SyncedStore.Find(guest.Mods, texture.Id, texture.Hash) != null,
            $"Trusted mods download: {ready.State} {ready.Message}");
        guest.Sync.Answer(["syncanswer", ready.Version.ToString(), "decline"]);
        guest.Shown.Next(check, "none");

        // Untrusted on the Mods page (its copy deleted with it): it waits for trust again; the texture is here.
        ModSettings.Untrust(guest.Mods, code.Id, code.Hash);
        SyncedStore.Forget(guest.Mods, code.Id, code.Hash);
        var untrusted = Refused(host, guest, check, "trust-c", "blocked");
        check(Action(untrusted, "test.code") == SyncAction.Trust && Action(untrusted, "test.texture") == SyncAction.Have, "Untrusted, it waits again: " + untrusted.Message);
        // Trust in other content of the same mod (another version's hash) is not trust in this one.
        ModSettings.SetTrusted(guest.Mods, new TrustedMod(code.Id, new string('0', 64), code.Name, code.Version, "Someone", ""));
        guest.Sync.Answer(["syncanswer", untrusted.Version.ToString(), "decline"]);
        guest.Shown.Next(check, "none");
        var otherHash = Refused(host, guest, check, "trust-d", "blocked");
        check(Action(otherHash, "test.code") == SyncAction.Trust, "Another hash trusted: this one still waits.");
        guest.Sync.Answer(["syncanswer", otherHash.Version.ToString(), "decline"]);
        guest.Shown.Next(check, "none");
        ModSettings.Untrust(guest.Mods, code.Id, new string('0', 64));
        ModSettings.Untrust(guest.Mods, texture.Id, texture.Hash);
        guest.Sync.Browse = _ => { };
        await host.Sync.DisposeAsync();
    }

    // The game's side over the live-edit pipe: the offer's lines, and its answer back.
    static async Task LiveDisplay(string root, Action<bool, string> check)
    {
        var runId = "modsync-" + Guid.NewGuid().ToString("N")[..8];
        await using var live = new LiveEditServer(runId);
        var answers = new BlockingCollection<string[]>();
        live.Received += parts => answers.Add(parts);
        live.Start();
        using var pipe = new NamedPipeClientStream(".", LiveEditServer.PipeName(runId), PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(5000);
        using var reader = new StreamReader(pipe, new UTF8Encoding(false));
        using var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
        await writer.WriteLineAsync($"hello\t{LiveEditServer.Protocol}\t{StatsSchema.Current.Build}\t{StatsSchema.Fingerprint}");
        var waited = Stopwatch.StartNew();
        while (!live.Connected && waited.Elapsed < TimeSpan.FromSeconds(5)) await Task.Delay(20);
        var lines = new List<string>();
        var game = Task.Run(async () =>
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                var parts = line.Split('\t');
                lock (lines) lines.Add(string.Join('\t', parts.Where((_, i) => i != 1)));
                await writer.WriteLineAsync(parts[1] + "\tok\tnow"); /* as the loader answers (live.c) */
                if (parts[0] == "syncstate") return;
            }
        });
        // (a url HellForge would not open never reaches the game, whatever the entry holds)
        var mod = new ModSetEntry("test.tab", "Name\twith a tab", "1", new string('a', 64), 1234, ModClass.Code, 0) { Url = "javascript:alert(1)" };
        var raw = new ModSetEntry("test.raw", "Raw", "2", new string('b', 64), 10, ModClass.Raw, 1) { Url = "https://example.com/raw" };
        var view = new SyncView(7, "offer", "Host\nName", 1244, 0, "Message", [new SyncItem(mod, SyncAction.Download, null), new SyncItem(raw, SyncAction.Trust, null)]);
        check(await new LiveSyncDisplay(live).ShowAsync(view, withMods: true), "The game took the offer.");
        await game;
        check(lines.SequenceEqual(["syncmod\t7\t0\tdownload\tcode\t1234\ttest.tab\tNamewith a tab\t1\t-", "syncmod\t7\t1\ttrust\traw\t10\ttest.raw\tRaw\t2\thttps://example.com/raw",
                "syncstate\t7\toffer\tHostName\t1244\t0\t2\tMessage"]),
            "The lines: " + string.Join(" | ", lines));
        await writer.WriteLineAsync("syncanswer\t7\tblock\ttest.tab");
        check(answers.TryTake(out var answer, TimeSpan.FromSeconds(5)) && answer!.SequenceEqual(["syncanswer", "7", "block", "test.tab"]), "The game's answer arrives.");
    }
}
