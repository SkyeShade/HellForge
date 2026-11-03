using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using HellForge.Core;
using HellForge.LocalBackend;

// Changing a running session's mods (LiveMods) against a game played by this test on the live pipe: a stats mod changed is
// sent as one change and, once the game reports it done, the session's mods, mod set and digest are the new ones; a code
// mod waits for the next session and sends nothing; the game's refusal leaves the session as it was; a result that comes
// after the wait still settles the session, and joins wait meanwhile; a change made with something left over is the
// session's too; a session playing a host's mods keeps them; no game connected, no change. A mod built on premium content
// this Steam account has not synced waits for a restart, from the Mods page and from a host's set, and reaches the game
// once the snapshot owns it.
static class LiveModsTests
{
    const string Rpm = """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}}}""";
    const uint PremiumWarbond = 610512218, PremiumToken = 2021502022, Knight = 2161995568;

    // The free warbond, one premium warbond with its token, and the Super Citizen Edition's package (with the Knight).
    static string ProgressionData(string folder)
    {
        var path = Path.Combine(folder, "progression-data.json");
        File.WriteAllText(path, "{\"seasonPassDetails\":{\"a\":{\"id32\":" + PremiumContent.FreeWarbond + ",\"premiumTokenMixIds\":[]},\"b\":{\"id32\":" +
            PremiumWarbond + ",\"premiumTokenMixIds\":[" + PremiumToken + "]}},\"itemPackages\":[{\"itemMixId32\":1542124658,\"items\":[{\"mixId\":" + Knight + "}]}]}");
        return path;
    }

    // A mod of stats edits: shared unless coop says otherwise (a stats mod is the player's own by itself, ModCatalog.Tier),
    // so that changing it changes what friends compare.
    static void Mod(string mods, string id, string? stats, string extra = "", string coop = "shared")
    {
        Directory.CreateDirectory(Path.Combine(mods, id));
        File.WriteAllText(Path.Combine(mods, id, ModCatalog.ManifestName), $$"""{"format":1,"id":"{{id}}","name":"{{id}} mod","version":"1","coop":"{{coop}}"{{extra}}}""");
        if (stats != null) File.WriteAllText(Path.Combine(mods, id, StatsData.FileName), stats);
    }

    public static async Task Run(string root, Action<bool, string> check)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "live-mods")).FullName;
        var mods = Path.Combine(folder, "mods");
        Mod(mods, "s1", Rpm);
        Mod(mods, "s2", Rpm.Replace("900", "800"));
        var sessionFile = Path.Combine(folder, SessionMods.FileName);
        var modSetFile = Path.Combine(folder, "modset.json");
        var started = ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(mods).Mods));
        SessionMods.Of(started, started, "").Write(sessionFile);
        ModSet.Of(started).Write(modSetFile);
        var before = ModSet.Of(started).Digest;

        var runId = "livemods-" + Guid.NewGuid().ToString("N")[..8];
        await using var live = new LiveEditServer(runId);
        live.Start();
        var log = new List<string>();
        // what the session's snapshot owns of premium content: nothing at first (mods without premium content go live anyway)
        var owned = PremiumContent.Nothing;
        var service = new LiveMods(live, mods, sessionFile, modSetFile, message => { lock (log) log.Add(message); })
            { ResultWait = TimeSpan.FromSeconds(1), Premium = () => owned };
        ModSet? changed = null;
        service.Changed += set => changed = set;
        var changing = new List<bool>();
        service.Changing += now => { lock (changing) changing.Add(now); };

        // no game yet: nothing is sent
        File.WriteAllText(Path.Combine(mods, "s1", StatsData.FileName), Rpm.Replace("900", "1000"));
        var unconnected = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(unconnected.Live && !unconnected.Applied && unconnected.Detail.Contains("not connected"), "A change went ahead with no game: " + unconnected.Detail);

        using var pipe = new NamedPipeClientStream(".", LiveEditServer.PipeName(runId), PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(5000);
        using var reader = new StreamReader(pipe, new UTF8Encoding(false));
        using var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
        await writer.WriteLineAsync($"hello\t{LiveEditServer.Protocol}\t{StatsSchema.Current.Build}\t{StatsSchema.Fingerprint}");
        var waited = Stopwatch.StartNew();
        while (!live.Connected && waited.Elapsed < TimeSpan.FromSeconds(5)) await Task.Delay(20);
        // the game: answers as live_mods.c does; refuses the next commit while `refuse` is set, keeps its result back while
        // `hold` is (the change's id in `held`), and reports `outcome`
        var lines = new List<string>();
        var refuse = false;
        var hold = false;
        string? held = null;
        var outcome = "ok";
        using var stop = new CancellationTokenSource();
        var game = Task.Run(async () =>
        {
            while (await reader.ReadLineAsync(stop.Token) is { } line)
            {
                var parts = line.Split('\t');
                lock (lines) lines.Add(parts[0] + (parts.Length > 3 && parts[0] != "modstat" ? " " + parts[3] : ""));
                if (parts[0] != "modcommit") { await writer.WriteLineAsync(parts[1] + "\tok\tnow"); continue; }
                if (refuse) { await writer.WriteLineAsync(parts[1] + "\trefused\tcoop\tanother player is in the game"); continue; }
                await writer.WriteLineAsync(parts[1] + "\tok\tqueued");
                if (hold) { lock (lines) held = parts[2]; continue; }
                await writer.WriteLineAsync($"modstate\t{parts[2]}\ts1\treplaced\t1\t0");
                await writer.WriteLineAsync($"modresult\t{parts[2]}\t{outcome}");
            }
        });

        // s1's stats changed: one change, then the session runs it
        var preview = service.Preview();
        check(preview.Live && preview.Changes.SequenceEqual(["s1 mod changed"]) && preview.Detail == "Applies now.", "The preview is wrong: " + preview.Detail);
        var applied = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        var now = SessionMods.Read(sessionFile);
        var s1 = ModCatalog.Load(mods).Mods.Single(m => m.Id == "s1");
        check(applied.Applied && applied.Digest != before && changed?.Digest == applied.Digest && now.Mods.Single(m => m.Id == "s1").Hash == ModSets.ContentHash(s1) &&
            ModSet.Parse(File.ReadAllText(modSetFile), local: true).Digest == applied.Digest, "The applied change was not recorded: " + applied.Detail + " " + string.Join(" | ", applied.Blocking));
        lock (lines) check(lines.SequenceEqual(["modbegin solo", "modorder 0", "modorder 1", "modreplace s1", "modstat", "modcommit"]), "The game's lines: " + string.Join(" | ", lines));
        check(service.Preview().Changes.Count == 0, "The applied change is still pending.");

        // a code mod waits for the next session, and nothing reaches the game
        Mod(mods, "c1", null, ",\"code\":{\"dll\":\"mod.dll\",\"sdk\":1}");
        File.WriteAllText(Path.Combine(mods, "c1", "mod.dll"), "synthetic dll");
        lock (lines) lines.Clear();
        var code = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(!code.Live && !code.Applied && code.Blocking.Any(b => b.Contains("code")) && code.Detail == "Applies to the next session.", "A code mod went live.");
        lock (lines) check(lines.Count == 0, "A code mod's change reached the game.");
        Directory.Delete(Path.Combine(mods, "c1"), true);

        // the game refuses: the session stays as it was
        File.WriteAllText(Path.Combine(mods, "s2", StatsData.FileName), Rpm.Replace("900", "700"));
        refuse = true;
        var refused = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(!refused.Applied && refused.Blocking.SequenceEqual(["Friends are in the game: another player is in the game."]) &&
            SessionMods.Read(sessionFile).Mods.SequenceEqual(now.Mods), "A refused change was recorded: " + string.Join(" | ", refused.Blocking));
        // joins waited for each change sent: the made one until Changed, the refused one until it ended
        lock (changing) check(changing.SequenceEqual([true, true, false]), "Joins did not wait for each change, or kept waiting: " + string.Join(',', changing));
        refuse = false;

        // the game reports after the wait: the change answers that it has not finished, joins keep waiting, and the result
        // when it comes settles the session (its mods, mod set and digest the new ones)
        hold = true;
        changed = null;
        var late = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(late.Live && !late.Applied && late.Blocking.SequenceEqual(["The game has not finished the change yet."]) && changed == null &&
            SessionMods.Read(sessionFile).Mods.SequenceEqual(now.Mods), "A change the game had not finished was recorded: " + late.Detail);
        lock (changing) check(changing[^1], "Joins did not wait for the unfinished change.");
        string lateTxn;
        lock (lines) lateTxn = held!;
        await writer.WriteLineAsync($"modresult\t{lateTxn}\tok");
        waited.Restart();
        while (changed == null && waited.Elapsed < TimeSpan.FromSeconds(5)) await Task.Delay(20);
        var s2 = ModCatalog.Load(mods).Mods.Single(m => m.Id == "s2");
        now = SessionMods.Read(sessionFile);
        check(changed != null && now.Mods.Single(m => m.Id == "s2").Hash == ModSets.ContentHash(s2) &&
            ModSet.Parse(File.ReadAllText(modSetFile), local: true).Digest == changed.Digest, "The game's late result did not settle the session.");
        hold = false;

        // a change the game made that cannot be recorded: joins check the new digest, they do not wait for the rest of the session
        File.WriteAllText(Path.Combine(mods, "s1", StatsData.FileName), Rpm.Replace("900", "1050"));
        File.Delete(modSetFile);
        Directory.CreateDirectory(modSetFile);
        changed = null;
        var unrecorded = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(unrecorded.Live && !unrecorded.Applied && unrecorded.Blocking.SequenceEqual(["The session's mods could not be recorded."]) && changed != null,
            "A change that could not be recorded left joins waiting: " + unrecorded.Detail);
        Directory.Delete(modSetFile);
        changed!.Write(modSetFile);

        // made with something of a mod taken out left: the session plays it (a restart makes sure), joins check the new digest
        File.WriteAllText(Path.Combine(mods, "s1", StatsData.FileName), Rpm.Replace("900", "1100"));
        outcome = "failed\tsomething of a mod taken out may be left; restart the session to be sure";
        changed = null;
        var leftover = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        now = SessionMods.Read(sessionFile);
        check(!leftover.Applied && leftover.Blocking.SequenceEqual(["Something of a mod taken out may be left."]) && changed?.Digest == leftover.Digest &&
            now.Mods.Single(m => m.Id == "s1").Hash == ModSets.ContentHash(ModCatalog.Load(mods).Mods.Single(m => m.Id == "s1")),
            "A change the game made with something left over was not recorded: " + string.Join(" | ", leftover.Blocking));
        outcome = "ok";

        // a mod built on premium content this Steam account has not synced waits for a restart (which locks its items), and
        // nothing reaches the game; once the snapshot owns it, it goes live
        var progression = ProgressionData(folder);
        Mod(mods, "p1", Rpm.Replace("900", "500"), ",\"premium\":[\"dlc:super-citizen-edition\"]");
        owned = PremiumContent.Load(progression, []);
        lock (lines) lines.Clear();
        var notOwned = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(!notOwned.Live && !notOwned.Applied && notOwned.Detail == "Applies to the next session." &&
            notOwned.Blocking.SequenceEqual(["p1 mod: it uses the Super Citizen Edition, which this Steam account has not synced, so it needs a restart"]) &&
            !service.Preview().Live && SessionMods.Read(sessionFile).Mods.All(m => m.Id != "p1"), "A premium mod not owned went live: " + string.Join(" | ", notOwned.Blocking));
        lock (lines) check(lines.Count == 0, "A premium mod not owned reached the game: " + string.Join(" | ", lines));
        owned = PremiumContent.Load(progression, [Knight]);
        var ownedNow = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        now = SessionMods.Read(sessionFile);
        check(ownedNow.Applied && now.Mods.Any(m => m.Id == "p1"), "A premium mod the snapshot owns did not go live: " + string.Join(" | ", ownedNow.Blocking));
        lock (lines) check(lines.Contains("modadd p1"), "The owned premium mod's lines: " + string.Join(" | ", lines));
        owned = PremiumContent.Nothing;

        // a session playing a host's mods keeps them: the Mods page applies to the next session
        new SessionMods(now.Mods, now.Skipped, "Host Diver").Write(sessionFile);
        var synced = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(!synced.Applied && synced.Blocking.Single().Contains("Host Diver's mods"), "A synced session took the Mods page's change.");

        // mod sync: the host plays s1 and s2 but not this player's s3, so the running game drops s3 and plays the host's set
        // (p1, built on premium content, stays as the session started with it, so it is not checked again)
        Mod(mods, "s3", Rpm.Replace("900", "600"));
        var own = ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(mods).Mods));
        SessionMods.Of(own, own, "").Write(sessionFile);
        var hostSet = ModSet.Of(own.Where(m => m.Id != "s3").ToList());
        var hostSession = new ModSyncSession("76561198000000001", "Host Diver", "connection", hostSet.Digest,
            [.. hostSet.Mods.Select(m => new ModSyncSession.Mod(m.Id, m.Hash, m.Order))]);
        check(service.CanApplySync(hostSession), "The host's set could not change the running game.");
        lock (lines) lines.Clear();
        var why = await service.ApplySyncAsync(hostSession, CancellationToken.None);
        var after = SessionMods.Read(sessionFile);
        check(why == null && after.SyncedWith == "Host Diver" && after.Mods.All(m => m.Id != "s3") && changed?.Digest == hostSet.Digest,
            "The host's set did not replace the session's: " + why);
        lock (lines) check(lines.Contains("modbegin sync") && lines.Contains("modremove s3"), "The game's lines: " + string.Join(" | ", lines));

        // mod sync: a host's set that brings a mod built on premium content this account has not synced is not taken live
        // (the session restarts with it, its items locked); once the snapshot owns it, it is
        Mod(mods, "p2", Rpm.Replace("900", "400"), ",\"premium\":[\"warbond:" + PremiumWarbond + "\"]");
        var premiumSet = ModSet.Of(ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(mods).Mods)).Where(m => m.Id != "s3").ToList());
        var premiumHost = new ModSyncSession("76561198000000001", "Host Diver", "connection", premiumSet.Digest,
            [.. premiumSet.Mods.Select(m => new ModSyncSession.Mod(m.Id, m.Hash, m.Order))]);
        lock (lines) lines.Clear();
        check(!service.CanApplySync(premiumHost), "A host's premium mod not owned could change the running game.");
        var heldBack = await service.ApplySyncAsync(premiumHost, CancellationToken.None);
        check(heldBack != null && heldBack.Contains("p2 mod: it uses warbond " + PremiumWarbond + ", which this Steam account has not synced", StringComparison.Ordinal) &&
            SessionMods.Read(sessionFile).Mods.All(m => m.Id != "p2"), "A host's premium mod not owned went live: " + heldBack);
        lock (lines) check(lines.Count == 0, "A host's premium mod not owned reached the game: " + string.Join(" | ", lines));
        owned = PremiumContent.Load(progression, [PremiumToken]);
        check(service.CanApplySync(premiumHost), "A host's premium mod the snapshot owns could not change the running game.");
        var ownedSync = await service.ApplySyncAsync(premiumHost, CancellationToken.None);
        check(ownedSync == null && SessionMods.Read(sessionFile).Mods.Any(m => m.Id == "p2") && changed?.Digest == premiumSet.Digest,
            "A host's premium mod the snapshot owns did not go live: " + ownedSync);

        // a per-player mod goes live too, and friends compare the same digest as before
        var mine = ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(mods).Mods));
        SessionMods.Of(mine, mine, "").Write(sessionFile);
        var shared = ModSet.Of(mine).Digest;
        Mod(mods, "pp", Rpm.Replace("900", "700"), coop: "per-player");
        lock (lines) lines.Clear();
        changed = null;
        var perPlayer = await service.ApplyAsync(null, sync: false, "", CancellationToken.None);
        check(perPlayer.Applied && perPlayer.Digest == shared && changed?.Digest == shared && SessionMods.Read(sessionFile).Mods.Any(m => m.Id == "pp"),
            "A per-player mod did not go live, or changed the digest: " + perPlayer.Detail + " " + string.Join(" | ", perPlayer.Blocking));
        lock (lines) check(lines.Contains("modadd pp"), "The per-player mod's lines: " + string.Join(" | ", lines));
        stop.Cancel();
        Console.WriteLine("PASS live mods: a stats mod changed live and recorded, a code mod held for the next session, the game's refusal, a late result settled (joins waiting meanwhile), " +
            "a change made with something left over, a synced session, no game, a host's set taken live, premium mods not owned held for a restart (Mods page and mod sync) and owned ones live, " +
            "a per-player mod live with the digest unchanged");
    }
}
