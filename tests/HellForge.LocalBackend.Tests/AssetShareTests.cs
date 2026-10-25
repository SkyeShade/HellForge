using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Text;
using HellForge.Core;
using HellForge.LocalBackend.Multiplayer;

// Assets in co-op, end to end: a host and two joiners over the in-memory network, each game a fake mod loader on its
// live-edit pipe that says which packages it holds ("packages") and records what it is asked to load ("preload"). Every
// game loads the lobby's packages less its own, again when a player's change; malformed lists are dropped. Each player's
// projectile swaps (from their Local Stats) reach every game, by their peer ("shots"), and so do their changes of
// projectile and damage rows ("rounds").
static class AssetShareTests
{
    const ulong HostId = 0x110000100000021, FirstId = 0x110000100000022, SecondId = 0x110000100000023;
    const string P1 = "00000000000000a1", P2 = "00000000000000b2", P3 = "00000000000000c3";

    sealed class Game : IAsyncDisposable
    {
        public readonly BlockingCollection<string> Preloads = new(), Shots = new(), Rounds = new();
        readonly BlockingCollection<string> outgoing = new();
        readonly CancellationTokenSource stop = new();
        readonly Task running;
        public volatile string Packages;

        public Game(string runId, string packages)
        {
            Packages = packages;
            running = Task.Run(() => Serve(runId));
        }

        // Says the game's packages changed: the new list goes up the pipe as the loader's own line would.
        public void Hold(string packages)
        {
            Packages = packages;
            outgoing.Add("packages\t" + packages);
        }

        async Task Serve(string runId)
        {
            while (!stop.IsCancellationRequested)
            {
                using var pipe = new NamedPipeClientStream(".", LiveEditServer.PipeName(runId), PipeDirection.InOut, PipeOptions.Asynchronous);
                try { await pipe.ConnectAsync(200, stop.Token); }
                catch (TimeoutException) { continue; }
                catch (OperationCanceledException) { return; }
                using var reader = new StreamReader(pipe, new UTF8Encoding(false));
                using var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
                var writing = new SemaphoreSlim(1, 1);
                async Task Write(string line) { await writing.WaitAsync(); try { await writer.WriteLineAsync(line); } finally { writing.Release(); } }
                await Write($"hello\t{LiveEditServer.Protocol}\t{StatsSchema.Current.Build}\t{StatsSchema.Fingerprint}");
                await Write("packages\t" + Packages);
                using var connected = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                var pushing = Task.Run(async () =>
                {
                    try { while (outgoing.TryTake(out var line, -1, connected.Token)) await Write(line); }
                    catch (Exception error) when (error is OperationCanceledException or IOException) { }
                });
                try
                {
                    while (await reader.ReadLineAsync(stop.Token) is { } line)
                    {
                        var parts = line.Split('\t');
                        if (parts[0] == "preload") Preloads.Add(parts[2]);
                        if (parts[0] == "shots") Shots.Add(parts[2]);
                        if (parts[0] == "rounds") Rounds.Add(parts[2]);
                        await Write(parts[1] + "\tok\tnow");
                    }
                }
                catch (Exception error) when (error is IOException or OperationCanceledException) { }
                connected.Cancel();
                await pushing;
            }
        }

        // The last list it was asked to load within the time, waiting until it equals `want` (or the time runs out).
        public string WaitFor(string want) => Wait(Preloads, want);
        public string WaitForShots(string want) => Wait(Shots, want);
        public string WaitForRounds(string want) => Wait(Rounds, want);
        static string Wait(BlockingCollection<string> lines, string want)
        {
            var last = "";
            for (var until = DateTime.UtcNow.AddSeconds(10); DateTime.UtcNow < until;)
                if (lines.TryTake(out var list, TimeSpan.FromMilliseconds(100)) && (last = list) == want) return last;
            return last;
        }

        public async ValueTask DisposeAsync()
        {
            stop.Cancel();
            await running;
        }
    }

    sealed class Machine : IAsyncDisposable
    {
        public readonly LobbyService Lobby;
        public readonly BlockingCollection<Frame> Inbox = new();
        public readonly StatsShare Stats;
        public readonly AssetShare Assets;
        public readonly Game Game;

        public Machine(InMemoryNetwork network, ulong id, string root, string name, string packages, ConcurrentQueue<string> log, string? localStats = null)
        {
            var link = network.Join(id);
            Lobby = new LobbyService(link, Inbox.Add, _ => { }, "mods");
            var mods = Directory.CreateDirectory(Path.Combine(root, "assets-" + name, "mods")).FullName;
            if (localStats != null)
                File.WriteAllText(Path.Combine(Directory.CreateDirectory(Path.Combine(mods, "zz.local.stats")).FullName, StatsShare.FileName), localStats);
            var runId = "assets-" + name + "-" + Guid.NewGuid().ToString("N")[..8];
            Stats = new StatsShare(link, Lobby, runId, mods, _ => "Host", line => log.Enqueue(name + ": " + line));
            var stats = Stats;
            Assets = new AssetShare(link, Lobby, Stats.Live, line => log.Enqueue(name + ": " + line), () => stats.LocalStats, () => stats.LobbyPlaysHostStats);
            link.Received += (peer, bytes) =>
            {
                var frame = Frame.Decode(bytes);
                if (frame.Type.StartsWith("stats.", StringComparison.Ordinal)) Stats.FromPeer(peer, frame);
                else if (frame.Type.StartsWith("assets.", StringComparison.Ordinal)) Assets.FromPeer(peer, frame);
                else Lobby.FromPeer(peer, frame);
            };
            Game = new Game(runId, packages);
        }

        public async ValueTask DisposeAsync()
        {
            await Game.DisposeAsync();
            await Assets.DisposeAsync();
            await Stats.DisposeAsync();
        }
    }

    public static async Task Run(string root, Action<bool, string> check)
    {
        var network = new InMemoryNetwork();
        var log = new ConcurrentQueue<string>();
        await using var host = new Machine(network, HostId, root, "host", P1, log);
        // the first joiner swapped the One-Two's and the Liberator's rounds to the Scorcher's (projectile 142) in its editor;
        // the Liberator's round comes from its ammunition (an entity delta of its ammunition's: stock 276), and the swap
        // counts for the weapon (0x968211C0033DCE64), with its stock round
        const string swap = """{"format":1,"weapons":{"AR/GL-21 One-Two":{"attack.primary.projectile":142},"AR-23 Liberator":{"ammunition.projectile":142}}}""";
        check(AssetShare.Swaps(swap) == "968211c0033dce64=142/276,a955c4ea6f6d4203=142" && AssetShare.Swaps(null) == "-",
            "A player's projectile swaps from their Local Stats: " + AssetShare.Swaps(swap));
        // a rounds feed's (the Double Freedom: WeaponRounds +64): the loader reads that weapon's normal round there ("/r")
        const string feed = """{"format":1,"weapons":{"DBS-2 Double Freedom":{"attack.feed_primary.projectile":142}}}""";
        check(AssetShare.Swaps(feed) == "72170a55a1f37ff1=142/r", "A rounds feed's projectile swap: " + AssetShare.Swaps(feed));
        // weapons where both members carry the round fire WeaponRounds +64 too (the Sweeper's primary, the Senator's feed)
        const string both = """{"format":1,"weapons":{"SG-97 Sweeper":{"attack.primary.projectile":142},"P-4 Senator":{"attack.feed_primary.projectile":142}}}""";
        check(AssetShare.Swaps(both) == "8d3d52a3b2f19402=142/r,dcd1c835407ef7ba=142/r", "Rounds swaps of weapons with both members: " + AssetShare.Swaps(both));
        // a weapon with a swap per round (the Purifier's partial and full charge), each by its stock type; an ammunition
        // option's delta reaches every weapon equipping it (the Knight's HOLLOW POINT is the Peacemaker's default)
        const string rounds = """{"format":1,"weapons":{"PLAS-101 Purifier":{"charge.level_1.projectile":142,"charge.level_2.projectile":150},"MP-98 Knight":{"ammunition.hollow_point.projectile":142}}}""";
        check(AssetShare.Swaps(rounds) == "5e4e5c2db6e44a2=142/337,9571ca51f0daf35b=142/337,fb3a19078694708a=142/129,fb3a19078694708a=150/296", "A swap per round, and an ammunition's for each weapon equipping it: " + AssetShare.Swaps(rounds));
        // the One-Two's grenade launcher is a weapon of its own (0x02CD7321CD8445F5), fed by rounds
        const string grenade = """{"format":1,"weapons":{"AR/GL-21 One-Two / underbarrel":{"attack.feed_primary.projectile":142}}}""";
        check(AssetShare.Swaps(grenade) == "2cd7321cd8445f5=142/r", "An underbarrel's projectile swap: " + AssetShare.Swaps(grenade));
        // a programmable round (the W.A.S.P.'s, ProjectileWeapon +0x240) is a swap of its own beside the weapon's normal one
        const string function = """{"format":1,"weapons":{"StA-X3 W.A.S.P. Launcher":{"attack.primary.projectile":142,"function_ammo.projectile":150}}}""";
        var functions = AssetShare.Swaps(function);
        check(functions.Split(',') is [var normal, var programmable] && normal.EndsWith("=142", StringComparison.Ordinal) &&
            programmable == normal[..normal.IndexOf('=')] + "=150/f", "A programmable round's projectile swap: " + functions);
        // round changes: the Liberator's armour penetration (its damage row 108, +0x0C) and velocity (its projectile row 276,
        // +0x20: 900 m/s), damage rows first; a swap is not one, nor is a whole-entity record
        const string changed = """{"format":1,"weapons":{"AR/GL-21 One-Two":{"attack.primary.projectile":142},"AR-23 Liberator":{"ammunition.projectile":142,"damage.ap_direct":4,"projectile.velocity":900}}}""";
        check(AssetShare.Rounds(changed) == "D108+c=4;P276+20=44610000" && AssetShare.Rounds(swap) == "-" && AssetShare.Rounds(null) == "-",
            "A player's changes of projectile and damage rows from their Local Stats: " + AssetShare.Rounds(changed));
        await using var first = new Machine(network, FirstId, root, "first", P2, log, changed);
        await using var second = new Machine(network, SecondId, root, "second", "-", log);

        // Alone, nothing is asked of any game.
        await Task.Delay(1500);
        check(host.Game.Preloads.Count == 0 && first.Game.Preloads.Count == 0, "Alone, no game is asked to load anything.");

        // Both join the host's lobby: every game loads the others' packages.
        host.Lobby.FromLocal(MultiplayerTests.Create("c1", HostId));
        host.Inbox.TryTake(out var created, TimeSpan.FromSeconds(5));
        var connection = created!.Text(4);
        first.Lobby.FromLocal(MultiplayerTests.Join("j1", FirstId, connection));
        second.Lobby.FromLocal(MultiplayerTests.Join("j2", SecondId, connection));
        var hostLoads = host.Game.WaitFor(P2);
        var firstLoads = first.Game.WaitFor(P1);
        var secondLoads = second.Game.WaitFor(P1 + "," + P2);
        check(hostLoads == P2, "Joined: the host's game loads the first joiner's package: " + hostLoads);
        check(firstLoads == P1, "Joined: the first joiner's game loads the host's package, not its own: " + firstLoads);
        check(secondLoads == P1 + "," + P2, "Joined: the second joiner's game loads both: " + secondLoads);
        // ... and every game is told the first joiner's swap, by its peer: its copies of their shots fire it
        var first16 = FirstId.ToString("x", System.Globalization.CultureInfo.InvariantCulture);
        var rule = first16 + ":968211c0033dce64=142/276," + first16 + ":a955c4ea6f6d4203=142";
        var hostShots = host.Game.WaitForShots(rule);
        var secondShots = second.Game.WaitForShots(rule);
        check(hostShots == rule && secondShots == rule, "Joined: the first joiner's projectile swap reaches the others' games: " + hostShots + " | " + secondShots);
        // ... and their round changes, by their peer: the others' copies of their shots fly and hit with them
        var changes = first16 + "=D108+c=4;P276+20=44610000";
        var hostRounds = host.Game.WaitForRounds(changes);
        var secondRounds = second.Game.WaitForRounds(changes);
        check(hostRounds == changes && secondRounds == changes, "Joined: the first joiner's round changes reach the others' games: " + hostRounds + " | " + secondRounds);

        // The first joiner applies a change that needs another package: it reaches every other game.
        first.Game.Hold(P2 + "," + P3);
        hostLoads = host.Game.WaitFor(P2 + "," + P3);
        secondLoads = second.Game.WaitFor(P1 + "," + P2 + "," + P3);
        check(hostLoads == P2 + "," + P3, "A joiner's new package reaches the host's game: " + hostLoads);
        check(secondLoads == P1 + "," + P2 + "," + P3, "... and the other joiner's: " + secondLoads);
        check(first.Game.WaitFor("never") == "", "The joiner itself is not asked again: its own are not friends'.");

        // A malformed list from a game is dropped, and what the others load stands.
        first.Game.Hold("not-a-package-list");
        check(host.Game.WaitFor("never") == "" && log.Any(l => l.Contains("malformed list of its packages")), "A malformed list is dropped and logged.");
        check(log.Any(l => l.StartsWith("host: Assets: sent the lobby's", StringComparison.Ordinal)) &&
            log.Any(l => l.StartsWith("first: Assets: sent this game's", StringComparison.Ordinal)), "Both sides logged what they sent.");
    }
}
