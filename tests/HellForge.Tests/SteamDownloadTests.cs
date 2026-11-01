using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using HellForge.Core;
using HellForge.SteamDownload;

// Setup's download of the supported build from Steam (HellForge.SteamDownload): planning from the pinned manifests,
// copying what Steam's installed build holds, writing and checking every file, continuing a stopped download, the
// fallbacks, and sign-in that keeps nothing. Steam is a fake here: no network, no account.
static class SteamDownloadTests
{
    const uint Bin = 553851, Data = 553853;

    static byte[] Bytes(int seed, int length)
    {
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }

    static string Sha1(byte[] bytes) => Convert.ToHexString(SHA1.HashData(bytes));

    // A file made of chunks: its manifest entry and its content.
    static (DepotFile Entry, byte[] Content) Made(string path, params byte[][] chunks)
    {
        var content = chunks.SelectMany(c => c).ToArray();
        ulong offset = 0;
        var listed = new List<DepotChunk>();
        foreach (var chunk in chunks)
        {
            listed.Add(new(Sha1(chunk), 0, offset, (uint)chunk.Length, (uint)chunk.Length));
            offset += (ulong)chunk.Length;
        }
        return (new DepotFile(path, (ulong)content.Length, 0, Sha1(content), listed), content);
    }

    // The fixture: a two-depot build of three files (one made of three chunks, one empty), and an older build in Steam's
    // folder. Its data/a.bin keeps the first chunk in place, the third chunk sits in another file, the second is new.
    sealed class Fixture
    {
        public readonly byte[] C0 = Bytes(1, 1000), C1 = Bytes(2, 1500), C2 = Bytes(3, 700), C3 = Bytes(4, 500), Other = Bytes(9, 1500), Lead = Bytes(8, 300);
        public readonly SupportedBuild Build;
        public readonly Dictionary<(uint, ulong), DepotManifest> Manifests = [];
        public readonly Dictionary<string, byte[]> Chunks = new(StringComparer.OrdinalIgnoreCase);
        public readonly Dictionary<string, byte[]> Contents = [];
        public readonly string Steam, Game, AppManifest, Home;

        public Fixture(string root, string name)
        {
            Home = Path.Combine(root, name);
            var a = Made("data/a.bin", C0, C1, C2);
            var b = Made("bin/b.dll", C3);
            var empty = Made("data/empty");
            foreach (var chunk in new[] { C0, C1, C2, C3 }) Chunks[Sha1(chunk)] = chunk;
            Contents["data/a.bin"] = a.Content;
            Contents["bin/b.dll"] = b.Content;
            Contents["data/empty"] = empty.Content;
            Build = new SupportedBuild(1, "553850", "100", "", "", [new SupportedDepot(Bin, "10"), new SupportedDepot(Data, "20")],
                [new("bin/b.dll", b.Content.Length, Sha1(b.Content), Bin), new("data/a.bin", a.Content.Length, Sha1(a.Content), Data),
                 new("data/empty", 0, Sha1([]), Data)]);
            Manifests[(Bin, 10)] = new(Bin, 10, false, [new DepotFile("bin", 0, 0x40, "", []), b.Entry, Made("tools/unused.exe", Bytes(5, 10)).Entry]);
            Manifests[(Data, 20)] = new(Data, 20, false, [a.Entry, empty.Entry]);

            // Steam's install of build 99: depot 553853 at manifest 21 (Steam cached it), depot 553851 at 11 (not cached)
            Steam = Path.Combine(Home, "Steam");
            Game = Path.Combine(Steam, "steamapps", "common", "Helldivers 2");
            var oldA = Made("data/a.bin", C0, Other);
            var moved = Made("data/other.bin", Lead, C2);
            Write("data/a.bin", oldA.Content);
            Write("data/other.bin", moved.Content);
            Write("bin/b.dll", b.Content);
            Directory.CreateDirectory(Path.Combine(Steam, "depotcache"));
            File.WriteAllBytes(Path.Combine(Steam, "depotcache", $"{Data}_21.manifest"), ManifestBytes(Data, 21, oldA.Entry, moved.Entry));
            AppManifest = Path.Combine(Steam, "steamapps", "appmanifest_553850.acf");
            File.WriteAllText(AppManifest, $$"""
                "AppState" { "appid" "553850" "StateFlags" "4" "installdir" "Helldivers 2" "buildid" "99"
                    "InstalledDepots" { "{{Bin}}" { "manifest" "11" } "{{Data}}" { "manifest" "21" } } }
                """);
        }

        void Write(string path, byte[] content)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(Game, path))!);
            File.WriteAllBytes(Path.Combine(Game, path), content);
        }

        public SteamGame Installed => new(AppManifest, new InstallReport(Game, AppManifest, new BuildPin("99", "", ""), 0, 0, []), false);

        public BuildDownloadJob Job(string destination, bool withSteam = true, IReadOnlyCollection<string>? only = null) =>
            new(Build, destination, withSteam ? Steam : null, withSteam ? Installed : null, only);

        // Every file of Steam's folder and its hash: the download only ever reads it.
        public string SteamState() => string.Join("\n", Directory.EnumerateFiles(Steam, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal)
            .Select(f => f + " " + Sha1(File.ReadAllBytes(f)) + " " + File.GetLastWriteTimeUtc(f).Ticks));

        public void CheckOutput(string destination, Action<bool, string> check)
        {
            foreach (var (path, content) in Contents)
                check(File.Exists(Path.Combine(destination, path)) && Sha1(File.ReadAllBytes(Path.Combine(destination, path))) == Sha1(content),
                    path + " is not the build's.");
            check(!Directory.EnumerateFiles(destination, "*", SearchOption.AllDirectories).Any(f => f.EndsWith(".partial") || f.EndsWith(".resume")),
                "A .partial or .resume file was left.");
        }
    }

    // Steam's manifest format (as <Steam>/depotcache holds it), every chunk of each file listed.
    static byte[] ManifestBytes(uint depot, ulong manifest, params DepotFile[] files)
    {
        static byte[] Varint(ulong value)
        {
            var bytes = new List<byte>();
            do { var current = (byte)(value & 0x7F); value >>= 7; bytes.Add(value != 0 ? (byte)(current | 0x80) : current); } while (value != 0);
            return [.. bytes];
        }
        static byte[] Number(int field, ulong value) => [.. Varint((ulong)(field << 3)), .. Varint(value)];
        static byte[] Blob(int field, byte[] value) => [.. Varint((ulong)(field << 3 | 2)), .. Varint((ulong)value.Length), .. value];
        static byte[] Section(uint magic, byte[] body) => [.. BitConverter.GetBytes(magic), .. BitConverter.GetBytes((uint)body.Length), .. body];
        var payload = new List<byte>();
        foreach (var file in files)
        {
            var entry = new List<byte>([.. Blob(1, Encoding.UTF8.GetBytes(file.Path.Replace('/', '\\'))), .. Number(2, file.Size), .. Number(3, file.Flags),
                .. Blob(5, Convert.FromHexString(file.Sha1))]);
            foreach (var chunk in file.Chunks)
                entry.AddRange(Blob(6, [.. Blob(1, Convert.FromHexString(chunk.Sha1)), .. Number(3, chunk.Offset), .. Number(4, chunk.OriginalSize),
                    .. Number(5, chunk.CompressedSize)]));
            payload.AddRange(Blob(1, [.. entry]));
        }
        var metadata = Number(1, depot).Concat(Number(2, manifest)).Concat(Number(4, 0)).ToArray();
        return [.. Section(0x71F617D0, [.. payload]), .. Section(0x1F4812BE, metadata), .. BitConverter.GetBytes(0x32C415ABu)];
    }

    // Steam as setup sees it once signed in. Denied: depots whose key it refuses; NoCode: manifests it grants no request
    // code for. Corrupt: chunks it sends wrong.
    sealed class FakeSteam(Fixture fixture) : IDepotService, IChunkFetcher
    {
        public readonly HashSet<uint> Denied = [];
        public readonly HashSet<ulong> NoCode = [];
        public readonly HashSet<string> Corrupt = new(StringComparer.OrdinalIgnoreCase);
        public readonly List<uint> KeysAsked = [];
        public int Fetched;
        public long FetchedBytes;
        public Func<DepotChunk, Task>? BeforeChunk;

        public Task<byte[]> DepotKeyAsync(uint app, uint depot, CancellationToken cancellation)
        {
            lock (KeysAsked) KeysAsked.Add(depot);
            return Denied.Contains(depot) ? throw new DepotAccessDeniedException(depot, "Steam answered AccessDenied") : Task.FromResult(new byte[] { 1, 2, 3 });
        }

        public Task<ulong> ManifestRequestCodeAsync(uint app, uint depot, ulong manifest, CancellationToken cancellation) =>
            Task.FromResult(NoCode.Contains(manifest) ? 0UL : 4242UL);

        public Task<DepotManifest> ManifestAsync(uint app, uint depot, ulong manifest, ulong requestCode, byte[] depotKey, CancellationToken cancellation) =>
            requestCode == 4242 && fixture.Manifests.TryGetValue((depot, manifest), out var found) ? Task.FromResult(found)
                : throw new HttpRequestException($"404 for {depot}_{manifest}");

        public Task<int> ChunkAsync(uint app, uint depot, DepotChunk chunk, byte[] depotKey, byte[] buffer, CancellationToken cancellation) =>
            FetchAsync(depot, chunk, buffer, cancellation);

        public async Task<int> FetchAsync(uint depot, DepotChunk chunk, byte[] buffer, CancellationToken cancellation)
        {
            if (BeforeChunk != null) await BeforeChunk(chunk);
            cancellation.ThrowIfCancellationRequested();
            var bytes = fixture.Chunks[chunk.Sha1];
            bytes.CopyTo(buffer, 0);
            if (Corrupt.Contains(chunk.Sha1)) buffer[0] ^= 0xFF;
            Interlocked.Increment(ref Fetched);
            Interlocked.Add(ref FetchedBytes, bytes.Length);
            return bytes.Length;
        }
    }

    static string Source(string relative) => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../src", relative));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The Steam download copies what Steam's installed build holds, downloads only the rest and checks every file", () =>
        {
            var fixture = new Fixture(root, "steam-download");
            var steam = new FakeSteam(fixture);
            var before = fixture.SteamState();
            var destination = Path.Combine(fixture.Home, "game");
            var progress = new List<AssembleProgress>();
            var planned = SteamBuildDownload.DownloadAsync(steam, fixture.Job(destination), null, progress.Add, default).GetAwaiter().GetResult();
            fixture.CheckOutput(destination, check);
            // the second chunk is the only new one; the first is in place in Steam's file, the third in another file of it
            // (Steam's cached manifest says where), b.dll is unchanged though depot 553851's manifest was not to be had
            check(steam.Fetched == 1 && steam.FetchedBytes == fixture.C1.Length, $"Downloaded {steam.Fetched} chunks, {steam.FetchedBytes} bytes.");
            check(planned.Plan.KnownLocalBytes == fixture.C0.Length + fixture.C2.Length, "Known local bytes: " + planned.Plan.KnownLocalBytes);
            check(planned.Notes.Count == 1 && planned.Notes[0].Contains("553851"), "Notes: " + string.Join(" | ", planned.Notes));
            var last = progress[^1];
            check(last.Done == fixture.Build.TotalBytes && last.Downloaded == fixture.C1.Length &&
                last.Copied == fixture.C0.Length + fixture.C2.Length + fixture.C3.Length, $"Progress: {last}");
            check(fixture.SteamState() == before, "Steam's folder changed.");
            // a second run keeps every file as it is and downloads nothing
            var stamp = File.GetLastWriteTimeUtc(Path.Combine(destination, "data/a.bin"));
            SteamBuildDownload.DownloadAsync(steam, fixture.Job(destination), null, null, default).GetAwaiter().GetResult();
            check(steam.Fetched == 1 && File.GetLastWriteTimeUtc(Path.Combine(destination, "data/a.bin")) == stamp, "A file in place was written again.");
        });

        test("Without Steam's install every chunk is downloaded, and --file fetches one file from its own depot only", () =>
        {
            var fixture = new Fixture(root, "steam-download-fresh");
            var steam = new FakeSteam(fixture);
            var destination = Path.Combine(fixture.Home, "game");
            SteamBuildDownload.DownloadAsync(steam, fixture.Job(destination, withSteam: false), null, null, default).GetAwaiter().GetResult();
            fixture.CheckOutput(destination, check);
            check(steam.Fetched == 4, "Chunks downloaded: " + steam.Fetched);
            var one = Path.Combine(fixture.Home, "one");
            var single = new FakeSteam(fixture);
            SteamBuildDownload.DownloadAsync(single, fixture.Job(one, only: ["bin/b.dll"]), null, null, default).GetAwaiter().GetResult();
            check(File.Exists(Path.Combine(one, "bin/b.dll")) && !File.Exists(Path.Combine(one, "data/a.bin")) && single.KeysAsked.SequenceEqual([Bin]),
                "Depots asked for one file: " + string.Join(",", single.KeysAsked));
            try
            {
                SteamBuildDownload.PlanAsync(single, fixture.Job(one, only: ["bin/not-there.dll"]), null, default).GetAwaiter().GetResult();
                check(false, "A file outside the build was planned.");
            }
            catch (ArgumentException) { }
        });

        test("A file that does not come out with the build's hash is never kept, and neither is a manifest that lists it otherwise", () =>
        {
            var fixture = new Fixture(root, "steam-download-bad");
            var steam = new FakeSteam(fixture);
            steam.Corrupt.Add(Sha1(fixture.C1));
            var destination = Path.Combine(fixture.Home, "game");
            reject(() => SteamBuildDownload.DownloadAsync(steam, fixture.Job(destination), null, null, default).GetAwaiter().GetResult());
            // what stays is the checked beginning, for the next run to copy from
            var partial = new FileInfo(Path.Combine(destination, "data/a.bin.partial"));
            check(!File.Exists(Path.Combine(destination, "data/a.bin")) && (!partial.Exists || partial.Length == fixture.C0.Length),
                "A file with a bad chunk was kept.");
            check(steam.Fetched == 3, "A bad chunk was not tried three times: " + steam.Fetched);
            // the pinned manifest must list each file as supported-build.json does, its chunks covering it once
            var a = fixture.Manifests[(Data, 20)].Files[0];
            var targets = new Dictionary<uint, DepotManifest> { [Data] = new(Data, 20, false, [a with { Sha1 = new string('0', 40) }]) };
            var file = fixture.Build.Files.Single(f => f.Path == "data/a.bin");
            reject(() => DepotPlanner.TargetOf(file, targets));
            targets[Data] = new(Data, 20, false, [a with { Chunks = [a.Chunks[0], a.Chunks[2]] }]);
            reject(() => DepotPlanner.TargetOf(file, targets));
            targets[Data] = new(Data, 20, false, [a with { Chunks = [a.Chunks[0], a.Chunks[1], a.Chunks[1] with { Offset = 1000 }, a.Chunks[2]] }]);
            reject(() => DepotPlanner.TargetOf(file, targets));
            targets[Data] = new(Data, 20, false, []);
            reject(() => DepotPlanner.TargetOf(file, targets));
        });

        test("A stopped download continues where it stopped: the part already written is copied, not downloaded again", () =>
        {
            var fixture = new Fixture(root, "steam-download-resume");
            var steam = new FakeSteam(fixture);
            var destination = Path.Combine(fixture.Home, "game");
            var targets = fixture.Manifests.Where(m => m.Key is (Bin, 10) or (Data, 20)).ToDictionary(m => m.Key.Item1, m => m.Value);
            var plan = DepotPlanner.Plan(fixture.Build.Files, targets, new(null, []));
            using var stop = new CancellationTokenSource();
            steam.BeforeChunk = chunk =>
            {
                if (chunk.Sha1 == Sha1(fixture.C2)) stop.Cancel();
                return Task.CompletedTask;
            };
            try
            {
                new DepotAssembler(steam, parallel: 1).AssembleAsync(plan, destination, null, stop.Token).GetAwaiter().GetResult();
                check(false, "The download did not stop.");
            }
            catch (OperationCanceledException) { }
            var partial = Path.Combine(destination, "data/a.bin.partial");
            check(File.Exists(partial) && new FileInfo(partial).Length == fixture.C0.Length + fixture.C1.Length, "The written beginning was not kept.");
            var again = new FakeSteam(fixture);
            new DepotAssembler(again, parallel: 1).AssembleAsync(plan, destination, null, default).GetAwaiter().GetResult();
            fixture.CheckOutput(destination, check);
            check(again.Fetched == 1 && again.FetchedBytes == fixture.C2.Length, $"After the stop, {again.Fetched} chunks were downloaded.");
        });

        test("Steam refusing the depot or the manifest (Family Sharing) offers the Steam console; a full drive asks for space", () =>
        {
            var fixture = new Fixture(root, "steam-download-denied");
            var destination = Path.Combine(fixture.Home, "game");
            var noCode = new FakeSteam(fixture);
            noCode.NoCode.Add(20);
            var keyDenied = new FakeSteam(fixture);
            keyDenied.Denied.Add(Bin);
            foreach (var steam in new[] { noCode, keyDenied })
            {
                try
                {
                    SteamBuildDownload.DownloadAsync(steam, fixture.Job(destination), null, null, default).GetAwaiter().GetResult();
                    check(false, "A refused depot was downloaded.");
                }
                catch (DepotAccessDeniedException denied)
                {
                    var failure = DownloadFailures.Describe(denied, "100");
                    check(failure is { Fallback: DownloadFallback.SteamConsole } && failure.Message.Contains("Family Sharing"), "Refused: " + failure?.Message);
                }
            }
            check(!Directory.Exists(destination) || !Directory.EnumerateFiles(destination, "*", SearchOption.AllDirectories).Any(), "A refused download wrote files.");
            check(DownloadFailures.Describe(new NotEnoughSpaceException("Not enough free space on C:\\."), "100")?.Fallback == DownloadFallback.FreeSpace, "A full drive.");
            check(DownloadFailures.Describe(new SteamSignInException("Steam sign-in did not work: the Steam Guard code was wrong."), "100")?.Fallback == DownloadFallback.Retry,
                "A failed sign-in.");
            var network = DownloadFailures.Describe(new HttpRequestException("x", new IOException("connection reset")), "100");
            check(network is { Fallback: DownloadFallback.SteamConsole } && network.Message.Contains("connection reset"), "A network failure: " + network?.Message);
            check(DownloadFailures.Describe(new ArgumentException("bug"), "100") == null, "A bug was taken for a download failure.");
        });

        test("Steam's cached manifest of the installed build is used only when it is that manifest with readable names", () =>
        {
            var fixture = new Fixture(root, "steam-download-cache");
            check(SteamBuildDownload.CachedManifest(fixture.Steam, Data, 21) is { Files.Count: 2 } cached && cached.Files[1].Chunks[1].Offset == 300,
                "The cached manifest was not read.");
            File.Copy(Path.Combine(fixture.Steam, "depotcache", $"{Data}_21.manifest"), Path.Combine(fixture.Steam, "depotcache", $"{Data}_22.manifest"));
            check(SteamBuildDownload.CachedManifest(fixture.Steam, Data, 22) == null && SteamBuildDownload.CachedManifest(fixture.Steam, Bin, 11) == null,
                "A manifest file named for another manifest, or a missing one, was used.");
            check(SteamBuildDownload.InstalledDepots(fixture.AppManifest).SequenceEqual([(Bin, 11UL), (Data, 21UL)]), "Installed depots.");
        });

        test("Sign-in keeps nothing: no password typed into HellForge, no token in text, no persistent session, no Steam Guard data, no SteamKit2 log", () =>
        {
            var scrubber = new SecretScrubber();
            scrubber.Add("hunter2-correct-horse");
            scrubber.Add("eyJ.refresh.token");
            check(scrubber.Clean("failed for hunter2-correct-horse with eyJ.refresh.token") == "failed for [hidden] with [hidden]", "The scrubber left a secret.");
            scrubber.Forget();
            check(scrubber.Clean("hunter2-correct-horse") == "hunter2-correct-horse", "The scrubber kept its secrets.");
            check(!SteamKit2.DebugLog.Enabled, "SteamKit2's debug log is on.");
            // the sign-in code itself: no line that shows, logs, writes or throws text names a password or a token, the
            // session is never persistent, nothing is saved, and Steam Guard data is never read back
            string[] sources = ["HellForge.SteamDownload/SignIn.cs", "HellForge.SteamDownload/SteamDepotSession.cs", "HellForge.SteamDownload/SteamCmd.cs",
                "HellForge.Cli/SteamDownloadCommand.cs", "HellForge.Launcher/SteamSignInFlow.cs"];
            var sink = new Regex(@"\b(Status|WriteLine|Write|Append|Exception|Log|Trace|Debug|Print|Show\w*)\s*\(|\.Text\s*=(?!\s*""""\s*;)|\$""");
            var secret = new Regex(@"\b\w*(Password|password|RefreshToken|refreshToken|AccessToken|accessToken|NewGuardData|GuardData)\b");
            foreach (var relative in sources)
            {
                var path = Source(relative);
                check(File.Exists(path), "Missing source " + relative);
                var text = File.ReadAllText(path);
                foreach (var line in text.Split('\n').Select(l => Regex.Replace(l, "\"(?:[^\"\\\\]|\\\\.)*\"", "\"\"")))
                    check(!(sink.IsMatch(line) && secret.IsMatch(line)), $"{relative}: a secret may reach the screen or a log: {line.Trim()}");
                check(!Regex.IsMatch(text, @"IsPersistentSession\s*=\s*true|ShouldRememberPassword\s*=\s*true|GuardData|SaveToFile|File\.Write|File\.Append|Registry"),
                    relative + " keeps something of the sign-in.");
            }
            var session = File.ReadAllText(Source("HellForge.SteamDownload/SteamDepotSession.cs"));
            check(session.Contains("IsPersistentSession = false") && session.Contains("ShouldRememberPassword = false"), "The session is not marked non-persistent.");
            // no password is ever typed into HellForge: no password sign-in, no password box, no hidden prompt
            var everything = string.Join("\n", new[] { "HellForge.SteamDownload", "HellForge.Cli", "HellForge.Launcher" }
                .SelectMany(project => Directory.EnumerateFiles(Source(project), "*.*", SearchOption.AllDirectories))
                .Where(path => path.EndsWith(".cs") || path.EndsWith(".axaml")).Where(path => !path.Contains("/obj/") && !path.Contains("\\obj\\"))
                .Select(File.ReadAllText));
            foreach (var gone in new[] { "BeginAuthSessionViaCredentialsAsync", "PasswordSignIn", "PasswordChar", "ReadHidden", "GetEmailCodeAsync", "IAuthenticator" })
                check(!everything.Contains(gone), "A password still goes into HellForge: " + gone);
        });

        test("Three ways to get the build, in order: the QR code (the default), SteamCMD with no phone, the Steam app", () =>
        {
            check(DownloadChoices.Default == DownloadChoice.QrCode && DownloadChoices.All.SequenceEqual([DownloadChoice.QrCode, DownloadChoice.SteamCmd, DownloadChoice.SteamClient]),
                "The choices or the default.");
            check(DownloadChoices.Offered(reachesSteam: true).SequenceEqual(DownloadChoices.All) &&
                DownloadChoices.Offered(reachesSteam: false).SequenceEqual([DownloadChoice.SteamClient]), "What setup offers inside the network namespace.");
            check(DownloadChoices.SignsIn(DownloadChoice.QrCode) && !DownloadChoices.SignsIn(DownloadChoice.SteamCmd) && !DownloadChoices.SignsIn(DownloadChoice.SteamClient),
                "Only the QR code signs in to HellForge.");
            check(DownloadChoices.All.Select(DownloadChoices.Title).Distinct().Count() == 3 && DownloadChoices.All.Select(DownloadChoices.Hint).Distinct().Count() == 3,
                "Two choices share their words.");
            var steamCmd = DownloadChoices.Explanation(DownloadChoice.SteamCmd, "100");
            check(DownloadChoices.Title(DownloadChoice.SteamCmd) == "No phone: sign in with SteamCMD" && steamCmd.Contains("Type your password in the SteamCMD window") &&
                steamCmd.Contains("Valve's own tool, not HellForge") && steamCmd.Contains("build 100") && steamCmd.Contains("removes SteamCMD"),
                "SteamCMD's words: " + steamCmd);
            var client = DownloadChoices.Explanation(DownloadChoice.SteamClient, "100");
            check(DownloadChoices.Title(DownloadChoice.SteamClient) == "Use the Steam app" && client.Contains("No sign-in in HellForge") && client.Contains("build 100") &&
                client.Contains("copies it"), "The Steam app's words: " + client);
            check(DownloadChoices.Explanation(DownloadChoice.QrCode, "100").Contains("never sees your password"), "The QR code's words.");
            check(DownloadFailures.Describe(new SteamCmdException("it could not be set up (no route)."), "100") is { Fallback: DownloadFallback.SteamConsole } failure &&
                failure.Message.Contains("Steam app"), "SteamCMD unavailable does not lead to the Steam app.");
        });

        test("SteamCMD's command line: the account name only, every pinned depot at its manifest, and quit; no password anywhere", () =>
        {
            var fixture = new Fixture(root, "steamcmd-arguments");
            var arguments = SteamCmd.Arguments("some_one1", fixture.Build);
            string[] expected = ["+@ShutdownOnFailedCommand", "1", "+login", "some_one1",
                .. fixture.Build.Depots.SelectMany(d => new[] { "+download_depot", fixture.Build.AppId, d.Id.ToString(), d.ManifestId }), "+quit"];
            check(arguments.SequenceEqual(expected), "Arguments: " + string.Join(" ", arguments));
            check(arguments.Count(a => a == "+login") == 1 && arguments[arguments.ToList().IndexOf("+login") + 1] == "some_one1" &&
                arguments[arguments.ToList().IndexOf("+login") + 2].StartsWith('+'), "Something follows the account name in +login (a password).");
            check(!arguments.Any(a => a.Contains("password", StringComparison.OrdinalIgnoreCase) || a.Contains("@NoPromptForPassword")), "A password setting on the command line.");
            var one = SteamCmd.Arguments("someone", fixture.Build, [Data]);
            check(one.Count(a => a == "+download_depot") == 1 && one.Contains(Data.ToString()) && !one.Contains(Bin.ToString()), "Only the depot asked for: " + string.Join(" ", one));
            foreach (var bad in new[] { "", "two words", "x\"+quit", "name+quit", "a;b", "name\nsecond", new string('a', 65) })
            {
                check(!SteamCmd.IsAccountName(bad), "Taken as an account name: " + bad);
                try
                {
                    SteamCmd.Arguments(bad, fixture.Build);
                    check(false, "A command line made for the account name " + bad);
                }
                catch (ArgumentException) { }
            }
            check(SteamCmd.IsAccountName("Some_Name_42"), "A real account name refused.");
            var steamCmd = SteamCmd.In(new InstallLayout(Path.Combine(fixture.Home, "HellForge")));
            check(steamCmd.Folder == Path.Combine(fixture.Home, "HellForge", "tools", "steamcmd"), "SteamCMD's folder: " + steamCmd.Folder);
            // never in a Steam library, whether or not Steam's own folder is known
            foreach (var library in new[] { Path.Combine(fixture.Home, "Steam", "steamapps", "x"), Path.Combine(fixture.Home, "Lib", "SteamApps", "common", "y") })
            {
                check(SteamCmd.InSteamapps(library), "Not seen as in steamapps: " + library);
                try
                {
                    _ = new SteamCmd(library);
                    check(false, "SteamCMD would live in a Steam library: " + library);
                }
                catch (ArgumentException) { }
            }
            check(!SteamCmd.InSteamapps(Path.Combine(fixture.Home, "steamappsx", "tools")), "A folder merely named like steamapps refused.");
            check(SteamCmd.WindowsArchive.Host == "steamcdn-a.akamaihd.net" && SteamCmd.LinuxArchive.AbsolutePath.EndsWith("/steamcmd_linux.tar.gz") &&
                SteamCmd.WindowsArchive.Scheme == "https", "SteamCMD is not fetched from Valve.");
        });

        test("SteamCMD's remembered sign-in is removed after it runs; SteamCMD and its download stay until the build is moved out", () =>
        {
            var fixture = new Fixture(root, "steamcmd-forget");
            var steamCmd = new SteamCmd(Path.Combine(fixture.Home, "tools", "steamcmd"));
            void Put(string relative) { var path = Path.Combine(steamCmd.Folder, relative); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, "x"); }
            string[] remembered = ["config/config.vdf", "config/loginusers.vdf", "ssfn1234567890", "userdata/1/config/localconfig.vdf", "logs/connection_log.txt",
                "home/Steam/config/config.vdf", "home/.steam/registry.vdf"];
            string[] kept = ["steamcmd.exe", "steamcmd.sh", "linux32/steamcmd", "package/steam_cmd_win32.manifest",
                $"steamapps/content/app_553850/depot_{Data}/data/a.bin"];
            foreach (var path in remembered.Concat(kept)) Put(path);
            steamCmd.ForgetSignIn();
            foreach (var path in remembered) check(!File.Exists(Path.Combine(steamCmd.Folder, path)), "Still remembered: " + path);
            foreach (var path in kept) check(File.Exists(Path.Combine(steamCmd.Folder, path)), "Removed too early: " + path);
            check(!Directory.Exists(Path.Combine(steamCmd.Folder, "config")) && !Directory.Exists(Path.Combine(steamCmd.Folder, "home")), "A remembered folder stayed.");
            steamCmd.ForgetSignIn(); // again, and with nothing there: no error
            steamCmd.Remove();
            check(!Directory.Exists(steamCmd.Folder), "SteamCMD was not removed.");
            steamCmd.ForgetSignIn();
            steamCmd.Remove();
            check(SteamCmd.Unfinished(5, []).Contains("Nothing of the sign-in was kept") && SteamCmd.Unfinished(5, []).Contains("type your password") &&
                SteamCmd.Unfinished(5, []).Contains("Steam app"), "A failed sign-in's words: " + SteamCmd.Unfinished(5, []));
        });

        test("SteamCMD is started only when Valve signed it: Valve and Valve Corp. are Valve's, nothing else is", () =>
        {
            static System.Security.Cryptography.X509Certificates.X500DistinguishedName Name(string text) => new(text);
            check(Authenticode.IsValve(Name("CN=Valve, OU=Digital ID Class 3 - Microsoft Software Validation v2, O=Valve, ST=Washington, C=US")),
                "steamcmd.exe's own signer refused.");
            check(Authenticode.IsValve(Name("CN=Valve Corp., O=Valve Corp., L=Bellevue, ST=Washington, C=US")), "Valve Corp. refused.");
            foreach (var other in new[] { "CN=Valve, O=Not Valve, C=US", "CN=Valve, C=US", "CN=x, O=Valve Software Ltd, C=US", "CN=x, O=Valve, O=Other, C=US",
                "CN=x, O=valve, C=US", "CN=x, O=\"Valve, Inc.\", C=US" })
                check(!Authenticode.IsValve(Name(other)), "Taken for Valve: " + other);
            check(!Authenticode.IsValve(null), "No signature taken for Valve.");
        });

        test("SteamCMD's download is picked up from its own folder (linux32 too), checked, and moved; a wrong file is never moved", () =>
        {
            var fixture = new Fixture(root, "steamcmd-pickup");
            foreach (var platform in new[] { "", "linux32" })
            {
                var steamCmd = new SteamCmd(Path.Combine(fixture.Home, "steamcmd-" + (platform.Length > 0 ? platform : "windows")));
                var content = Path.Combine(steamCmd.Folder, platform, "steamapps", "content", "app_553850");
                void Put(string path, byte[] bytes)
                {
                    var file = fixture.Build.Files.Single(f => f.Path == path);
                    var target = Path.Combine(content, $"depot_{file.Depot}", path);
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    File.WriteAllBytes(target, bytes);
                }
                Put("bin/b.dll", fixture.Contents["bin/b.dll"]);
                Put("data/empty", []);
                check(!SteamDepotDownload.IsComplete(steamCmd.Folder, fixture.Build), "Half a download counted as complete.");
                check(SteamCmd.Unfinished(0, SteamDepotDownload.Status(steamCmd.Folder, fixture.Build)).Contains("Start it again to continue"), "A half download's words.");
                Put("data/a.bin", fixture.Contents["data/a.bin"]);
                check(SteamDepotDownload.IsComplete(steamCmd.Folder, fixture.Build), $"SteamCMD's download ({platform}) was not found.");
                // a wrong file: refused, nothing of it moved
                var wrong = Path.Combine(fixture.Home, "steamcmd-wrong-" + platform.Length);
                File.WriteAllBytes(Path.Combine(content, $"depot_{Bin}", "bin/b.dll"), Bytes(78, fixture.C3.Length));
                reject(() => GameCopier.MoveFromDepots(steamCmd.Folder, wrong, fixture.Build));
                check(!File.Exists(Path.Combine(wrong, "bin/b.dll")), "A wrong file was moved.");
                Put("bin/b.dll", fixture.Contents["bin/b.dll"]);
                var destination = Path.Combine(fixture.Home, "steamcmd-game-" + platform.Length);
                GameCopier.MoveFromDepots(steamCmd.Folder, destination, fixture.Build);
                fixture.CheckOutput(destination, check);
                check(!File.Exists(Path.Combine(content, $"depot_{Data}", "data/a.bin")), "The file was copied, not moved.");
                reject(() => GameCopier.MoveFromDepots(steamCmd.Folder, Path.Combine(steamCmd.Folder, "game"), fixture.Build));
                steamCmd.Remove();
            }
        });

        test("The Steam client's download is picked up where Steam put it, past an empty leftover folder, and copied verified", () =>
        {
            var fixture = new Fixture(root, "steam-client-download");
            var steamRoot = Path.Combine(fixture.Home, "Client");
            var content = Path.Combine(steamRoot, "steamapps", "content", "app_553850");
            // an earlier download's leftovers in the usual place, the real download under steamrt64 (Linux's 64-bit client)
            Directory.CreateDirectory(Path.Combine(content, $"depot_{Bin}"));
            Directory.CreateDirectory(Path.Combine(content, $"depot_{Data}"));
            var real = Path.Combine(steamRoot, "steamrt64", "steamapps", "content", "app_553850");
            void Put(string path, byte[] bytes)
            {
                var file = fixture.Build.Files.Single(f => f.Path == path);
                var target = Path.Combine(real, $"depot_{file.Depot}", path);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.WriteAllBytes(target, bytes);
            }
            Put("bin/b.dll", fixture.Contents["bin/b.dll"]);
            Put("data/empty", []);
            Put("data/a.bin", fixture.Contents["data/a.bin"][..1000]);
            // still downloading: not complete, and the file it waits for is named
            check(!SteamDepotDownload.IsComplete(steamRoot, fixture.Build), "A download still under way counted as complete.");
            var waiting = SteamDepotDownload.Status(steamRoot, fixture.Build).SelectMany(s => s.Waiting).ToList();
            check(waiting.Count == 1 && waiting[0].Path == "data/a.bin" && waiting[0].Have == 1000, "Waiting for: " + string.Join(", ", waiting));
            Put("data/a.bin", fixture.Contents["data/a.bin"]);
            check(SteamDepotDownload.IsComplete(steamRoot, fixture.Build), "The finished download was not found.");
            check(SteamDepotDownload.DepotFolder(steamRoot, fixture.Build, Data) == Path.Combine(real, $"depot_{Data}"), "The empty leftover folder won.");
            var destination = Path.Combine(fixture.Home, "client-game");
            GameCopier.CopyFromDepots(steamRoot, destination, fixture.Build);
            fixture.CheckOutput(destination, check);
            // a file the Steam client wrote wrong is never copied
            Put("bin/b.dll", Bytes(77, fixture.C3.Length));
            reject(() => GameCopier.CopyFromDepots(steamRoot, Path.Combine(fixture.Home, "client-game-2"), fixture.Build));
        });

        test("The Steam download's libraries ship with their licences, and the notices name the versions referenced", () =>
        {
            var licences = Path.Combine(AppContext.BaseDirectory, "licences");
            foreach (var name in new[] { "THIRD-PARTY-NOTICES.txt", "SteamKit2-LICENSE.txt", "Apache-2.0.txt", "protobuf-net-LICENSE.txt", "QRCoder-LICENSE.txt",
                "ZstdSharp.Port-LICENSE.txt", "zstd-LICENSE.txt", "System.IO.Hashing-LICENSE.txt", "System.IO.Hashing-THIRD-PARTY-NOTICES.txt" })
                check(File.Exists(Path.Combine(licences, name)), "Missing licences/" + name);
            check(File.ReadAllText(Path.Combine(licences, "SteamKit2-LICENSE.txt")).Contains("GNU LESSER GENERAL PUBLIC LICENSE"), "SteamKit2's licence is not the LGPL text.");
            var notices = File.ReadAllText(Path.Combine(licences, "THIRD-PARTY-NOTICES.txt"));
            foreach (var reference in Regex.Matches(File.ReadAllText(Source("HellForge.SteamDownload/HellForge.SteamDownload.csproj")),
                "PackageReference Include=\"([^\"]+)\" Version=\"([^\"]+)\"").Cast<Match>())
                check(notices.Contains($"{reference.Groups[1].Value} {reference.Groups[2].Value}"), $"The notices do not name {reference.Groups[1].Value} {reference.Groups[2].Value}.");
            foreach (var library in new[] { "SteamKit2", "protobuf-net", "protobuf-net.Core", "ZstdSharp", "System.IO.Hashing", "QRCoder" })
                check(notices.Contains(library + ".dll"), "The notices do not name " + library + ".dll");
        });

        test("The sign-in QR code has its quiet zone and finder patterns, and draws in a terminal", () =>
        {
            var modules = QrCode.Modules("https://s.team/q/1/1234567890123456789");
            var size = modules.GetLength(0);
            check(size == modules.GetLength(1) && size >= 29, "Size " + size);
            check(!modules[0, 0] && !modules[3, 3] && modules[4, 4] && modules[4, size - 5] && modules[size - 5, 4],
                "The finder patterns are not where they belong.");
            var lines = QrCode.Terminal("https://s.team/q/1/1234567890123456789").Split('\n', StringSplitOptions.RemoveEmptyEntries);
            check(lines.Length == (size + 1) / 2 && lines.All(l => l.StartsWith("\u001b[97;40m")), "Terminal lines: " + lines.Length);
        });
    }
}
