using System.Collections.Concurrent;
using System.Text.Json;
using HellForge.Core.GameData;
using QRCoder;
using SteamKit2;
using SteamKit2.Authentication;
using SteamKit2.CDN;

// Reads any build of the game straight from Steam's depots, for an account that owns it: a depot manifest lists every
// file with its ~1 MB chunks, and a file opens as a seekable stream that downloads (and decrypts) only the chunks it
// reads. Chunks are cached on disk by their SHA-1, so a chunk shared by many builds is downloaded once, and a capped
// in-memory cache keeps the most recent ones. Nothing is written into a game folder and nothing is installed.
//
// Login is Steam's QR flow (the Steam mobile app approves it); the refresh token is kept in the user's config folder
// (~/.config/hellforge-migrator/steam-login.json, readable by the user only), never in the repository.
sealed class SteamDepots : IDisposable
{
    public const uint AppId = 553850;
    static readonly string ConfigFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "hellforge-migrator");
    static readonly string LoginFile = Path.Combine(ConfigFolder, "steam-login.json");
    // HF_MIGRATOR_CACHE moves the manifest and chunk cache (it can grow to the disk cache limit).
    public static readonly string CacheFolder = Environment.GetEnvironmentVariable("HF_MIGRATOR_CACHE") is { Length: > 0 } custom
        ? custom
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "hellforge-migrator");

    readonly SteamClient client = new();
    readonly CallbackManager callbacks;
    readonly SteamUser user;
    readonly SteamApps apps;
    readonly SteamContent content;
    readonly Client cdn;
    readonly Thread pump;
    volatile bool running = true;
    readonly ConcurrentDictionary<uint, byte[]> depotKeys = new();
    IReadOnlyList<Server> servers = [];
    int nextServer;
    public long ChunksDownloaded, BytesDownloaded, ChunksFromDisk;
    public long DiskCacheLimit { get; set; } = 40L << 30;

    SteamDepots()
    {
        callbacks = new CallbackManager(client);
        user = client.GetHandler<SteamUser>()!;
        apps = client.GetHandler<SteamApps>()!;
        content = client.GetHandler<SteamContent>()!;
        cdn = new Client(client);
        pump = new Thread(() => { while (running) callbacks.RunWaitCallbacks(TimeSpan.FromMilliseconds(200)); }) { IsBackground = true };
        pump.Start();
    }

    sealed record SavedLogin(string AccountName, string RefreshToken);

    public enum LoginMethod { Saved, Qr, Password }

    // Connects and logs in: with the saved token, or (interactively, once) by one of the two login methods:
    //   Qr        a QR code to scan in the Steam mobile app.
    //   Password  account name and password typed here, then the Steam Guard code Steam emails (or the code the
    //             authenticator shows, or an approval in the app), as Steam asks for it. Nothing is stored but the token.
    public static async Task<SteamDepots> Connect(LoginMethod method = LoginMethod.Saved)
    {
        // The account name and password are asked for before connecting: Steam drops a connection left idle while
        // someone types.
        (string Name, string Password)? credentials = method == LoginMethod.Password ? AskCredentials() : null;
        var steam = new SteamDepots();
        await steam.ConnectClient();
        SavedLogin? saved = method == LoginMethod.Saved && File.Exists(LoginFile) ? JsonSerializer.Deserialize<SavedLogin>(File.ReadAllText(LoginFile)) : null;
        if (saved is null)
        {
            if (method == LoginMethod.Saved) throw new InvalidOperationException("Not logged in to Steam: run the migrator's login command once (login --qr or login --password).");
            AuthPollResult result;
            try
            {
                result = method == LoginMethod.Qr ? await QrLogin(steam) : await PasswordLogin(steam, credentials!.Value);
            }
            catch (AuthenticationException e)
            {
                throw new InvalidOperationException($"Steam login did not complete ({e.Result}): {e.Message}. Run the login command again.");
            }
            catch (TaskCanceledException)
            {
                throw new InvalidOperationException("Steam login timed out before it was approved. Run the login command again.");
            }
            saved = new SavedLogin(result.AccountName, result.RefreshToken);
            Directory.CreateDirectory(ConfigFolder);
            File.WriteAllText(LoginFile, JsonSerializer.Serialize(saved));
            if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(LoginFile, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        await steam.LogOn(saved);
        return steam;
    }

    static async Task<AuthPollResult> QrLogin(SteamDepots steam)
    {
        var session = await steam.client.Authentication.BeginAuthSessionViaQRAsync(new AuthSessionDetails
        {
            DeviceFriendlyName = "HellForge asset migrator",
            IsPersistentSession = true,
        });
        void Show()
        {
            using var data = new QRCodeGenerator().CreateQrCode(session.ChallengeURL, QRCodeGenerator.ECCLevel.L);
            Console.WriteLine(new AsciiQRCode(data).GetGraphicSmall());
            Console.WriteLine("Scan this with the Steam mobile app (Steam Guard > scan a QR code) and approve the login. A new code replaces it every few minutes.");
        }
        session.ChallengeURLChanged = Show;
        Show();
        return await session.PollingWaitForResultAsync();
    }

    static (string, string) AskCredentials()
    {
        Console.Write("Steam account name: ");
        var name = Console.ReadLine()?.Trim();
        if (string.IsNullOrEmpty(name)) throw new InvalidOperationException("No account name given.");
        Console.Write("Password (not shown): ");
        return (name, ReadHidden());
    }

    static async Task<AuthPollResult> PasswordLogin(SteamDepots steam, (string Name, string Password) credentials)
    {
        var (name, password) = credentials;
        var session = await steam.client.Authentication.BeginAuthSessionViaCredentialsAsync(new AuthSessionDetails
        {
            Username = name,
            Password = password,
            DeviceFriendlyName = "HellForge asset migrator",
            IsPersistentSession = true,
            // Asks on this console for the code Steam emails, or the authenticator's code, or waits for the app's approval.
            Authenticator = new UserConsoleAuthenticator(),
        });
        return await session.PollingWaitForResultAsync();
    }

    static string ReadHidden()
    {
        if (Console.IsInputRedirected) return Console.ReadLine() ?? "";
        var text = new System.Text.StringBuilder();
        while (true)
        {
            var key = Console.ReadKey(intercept: true);
            if (key.Key == ConsoleKey.Enter) break;
            if (key.Key == ConsoleKey.Backspace) { if (text.Length > 0) text.Length--; continue; }
            if (!char.IsControl(key.KeyChar)) text.Append(key.KeyChar);
        }
        Console.WriteLine();
        return text.ToString();
    }

    async Task ConnectClient()
    {
        var connected = new TaskCompletionSource();
        using var a = callbacks.Subscribe<SteamClient.ConnectedCallback>(_ => connected.TrySetResult());
        using var b = callbacks.Subscribe<SteamClient.DisconnectedCallback>(_ => connected.TrySetException(new IOException("Could not connect to Steam.")));
        client.Connect();
        await connected.Task.WaitAsync(TimeSpan.FromSeconds(30));
    }

    async Task LogOn(SavedLogin login)
    {
        // A login approved in the app (or a code typed) can take minutes, and Steam may have dropped the connection.
        if (!client.IsConnected) await ConnectClient();
        var done = new TaskCompletionSource<EResult>();
        using var a = callbacks.Subscribe<SteamUser.LoggedOnCallback>(cb => done.TrySetResult(cb.Result));
        user.LogOn(new SteamUser.LogOnDetails { Username = login.AccountName, AccessToken = login.RefreshToken, ShouldRememberPassword = true });
        var result = await done.Task.WaitAsync(TimeSpan.FromSeconds(30));
        // The saved login is never deleted here: a refusal can be passing (rate limits, Steam maintenance), and a new
        // login overwrites it anyway.
        if (result != EResult.OK)
            throw new InvalidOperationException($"Steam refused the saved login: EResult {result} ({(int)result}). " +
                (result is EResult.InvalidPassword or EResult.AccessDenied or EResult.Expired or EResult.Revoked
                    ? "The saved token is no longer accepted; run the login command again."
                    : "This may pass; try again later before logging in again."));
        servers = (await content.GetServersForSteamPipe()).Where(s => s.Type is "SteamCache" or "CDN").ToList();
        if (servers.Count == 0) throw new IOException("Steam gave no content servers.");
    }

    async Task<byte[]> DepotKey(uint depot)
    {
        if (depotKeys.TryGetValue(depot, out var key)) return key;
        var result = await apps.GetDepotDecryptionKey(depot, AppId);
        if (result.Result != EResult.OK) throw new InvalidOperationException($"Steam gave no key for depot {depot} ({result.Result}): does this account own the game?");
        return depotKeys[depot] = result.DepotKey;
    }

    Server NextServer() => servers[Interlocked.Increment(ref nextServer) % servers.Count];

    public Task<byte[]> Key(uint depot) => DepotKey(depot);
    public IReadOnlyList<Server> Servers => servers;

    // A manifest from the disk cache alone, without logging in (null when it is not cached or its names are encrypted).
    public static DepotManifest? Cached(uint depot, ulong manifest)
    {
        var file = Path.Combine(CacheFolder, "manifests", $"{depot}_{manifest}.manifest");
        var m = File.Exists(file) ? DepotManifest.LoadFromFile(file) : null;
        return m is { FilenamesEncrypted: false } ? m : null;
    }

    // A depot manifest (cached on disk after the first download), with its file names decrypted.
    public async Task<DepotManifest> Manifest(uint depot, ulong manifest, string branch = "public")
    {
        var file = Path.Combine(CacheFolder, "manifests", $"{depot}_{manifest}.manifest");
        if (await FromCache(depot, file) is { } cached) return cached;
        var key = await DepotKey(depot);
        Exception? last = null;
        foreach (var b in branch == "public" ? new[] { "public" } : new[] { branch, "public" })
            for (var attempt = 0; attempt < 4; attempt++)
            {
                try { return await DownloadManifest(depot, manifest, b, key, file); }
                catch (Exception e) when (e is not OperationCanceledException) { last = e; }
            }
        throw new IOException($"Could not download manifest {manifest} of depot {depot}: {last?.Message}");
    }

    // The manifest from the disk cache, with its file names decrypted, or null when the cache does not have it.
    async Task<DepotManifest?> FromCache(uint depot, string file)
    {
        if (!File.Exists(file)) return null;
        var cached = DepotManifest.LoadFromFile(file);
        if (cached is not null && cached.FilenamesEncrypted) cached.DecryptFilenames(await DepotKey(depot));
        return cached;
    }

    // One download of the manifest for a branch, its file names decrypted, saved to the disk cache.
    async Task<DepotManifest> DownloadManifest(uint depot, ulong manifest, string branch, byte[] key, string file)
    {
        var code = await content.GetManifestRequestCode(depot, AppId, manifest, branch);
        if (Environment.GetEnvironmentVariable("HF_STEAM_DEBUG") == "1") Console.Error.WriteLine($"  manifest {manifest} branch {branch}: request code {code}");
        var m = await cdn.DownloadManifestAsync(depot, manifest, code, NextServer(), key);
        if (m.FilenamesEncrypted) m.DecryptFilenames(key);
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        m.SaveToFile(file);
        return m;
    }

    // One chunk's bytes (decrypted and decompressed), from the disk cache or from Steam.
    // Chunks this PC already has in an installed build's files: read from there instead of downloaded.
    public LocalChunks Local { get; } = new();

    public async Task<byte[]> Chunk(uint depot, DepotManifest.ChunkData chunk)
    {
        var id = Convert.ToHexString(chunk.ChunkID!).ToLowerInvariant();
        var file = Path.Combine(CacheFolder, "chunks", id[..2], id);
        if (File.Exists(file))
        {
            var bytes = await File.ReadAllBytesAsync(file);
            if (bytes.Length == chunk.UncompressedLength) { Interlocked.Increment(ref ChunksFromDisk); return bytes; }
        }
        if (Local.TryRead(chunk) is { } local) return local;
        var key = await DepotKey(depot);
        Exception? last = null;
        for (var attempt = 0; attempt < 6; attempt++)
        {
            try
            {
                var buffer = new byte[chunk.UncompressedLength];
                var written = await cdn.DownloadDepotChunkAsync(depot, chunk, NextServer(), buffer, key);
                if (written != buffer.Length) throw new IOException($"chunk {id}: {written} of {buffer.Length} bytes");
                Interlocked.Increment(ref ChunksDownloaded);
                Interlocked.Add(ref BytesDownloaded, chunk.CompressedLength);
                Directory.CreateDirectory(Path.GetDirectoryName(file)!);
                var temp = file + "." + Environment.CurrentManagedThreadId + ".tmp";
                await File.WriteAllBytesAsync(temp, buffer);
                File.Move(temp, file, true);
                return buffer;
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                last = e;
                // A content server under load answers 503 for a while: back off (0.5 s doubling to 16 s), next server.
                await Task.Delay(500 << attempt);
            }
        }
        throw new IOException($"Could not download chunk {id} of depot {depot}: {last?.Message}");
    }

    // Keeps the disk chunk cache under its limit, oldest files first.
    public void TrimCache()
    {
        var folder = Path.Combine(CacheFolder, "chunks");
        if (!Directory.Exists(folder)) return;
        var files = new DirectoryInfo(folder).EnumerateFiles("*", SearchOption.AllDirectories).OrderBy(f => f.LastAccessTimeUtc).ToList();
        var total = files.Sum(f => f.Length);
        foreach (var f in files)
        {
            if (total <= DiskCacheLimit) break;
            total -= f.Length;
            f.Delete();
        }
    }

    public void Dispose()
    {
        running = false;
        // SteamKit2 3.5.0's WebSocket disconnect calls RunSynchronously on an async task and throws; the connection is
        // being dropped either way.
        try { client.Disconnect(); }
        catch (InvalidOperationException) { }
    }
}

// One depot manifest's files under a folder (the game's "data"), as a game data folder the readers can open.
sealed class DepotFiles : IDataFiles
{
    readonly SteamDepots steam;
    readonly uint depot;
    readonly Dictionary<string, DepotManifest.FileData> files = new(StringComparer.OrdinalIgnoreCase);
    // Chunks recently read, shared by every stream of this depot: a resource's parts are read one after another.
    readonly ConcurrentDictionary<string, Lazy<Task<byte[]>>> recent = new();
    readonly ConcurrentQueue<string> order = new();
    const int Recent = 384;

    public string Name { get; }
    public DepotManifest Manifest { get; }

    public DepotFiles(SteamDepots steam, DepotManifest manifest, string folder = "data")
    {
        this.steam = steam;
        Manifest = manifest;
        depot = manifest.DepotID;
        Name = $"depot {depot} manifest {manifest.ManifestGID}";
        var prefix = folder.Length == 0 ? "" : folder.TrimEnd('/') + "/";
        foreach (var f in manifest.Files ?? [])
        {
            var path = f.FileName.Replace('\\', '/');
            if (path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && !path[prefix.Length..].Contains('/') && (f.Flags & EDepotFileFlag.Directory) == 0)
                files[path[prefix.Length..]] = f;
        }
    }

    public bool Exists(string name) => files.ContainsKey(name);
    public IEnumerable<string> List() => files.Keys;
    public DepotManifest.FileData File(string name) => files[name];
    public Stream Open(string name) => new DepotStream(this, files.TryGetValue(name, out var f) ? f : throw new FileNotFoundException(name));

    // The file's SHA-1 as the manifest lists it: the same bytes in another build have the same hash.
    string? IDataFiles.Hash(string name) => Hash(name);
    public string? Hash(string name) => files.TryGetValue(name, out var f) && f.FileHash is { } h ? Convert.ToHexString(h).ToLowerInvariant() : null;

    // Downloads the first chunk of each of these files (where their tables are) into the disk cache, many at a time.
    public void PrefetchFirstChunks(IEnumerable<string> names, int parallel = 32)
    {
        var firsts = names.Where(files.ContainsKey).Select(n => files[n].Chunks.OrderBy(c => c.Offset).FirstOrDefault()).OfType<DepotManifest.ChunkData>().ToList();
        Parallel.ForEachAsync(firsts, new ParallelOptions { MaxDegreeOfParallelism = parallel }, async (c, _) => await steam.Chunk(depot, c)).GetAwaiter().GetResult();
    }

    internal byte[] Chunk(DepotManifest.ChunkData chunk, IEnumerable<DepotManifest.ChunkData> next)
    {
        var id = Convert.ToHexString(chunk.ChunkID!);
        var task = Get(chunk);
        foreach (var ahead in next) _ = Get(ahead).Value; // read-ahead
        return task.Value.GetAwaiter().GetResult();

        Lazy<Task<byte[]>> Get(DepotManifest.ChunkData c)
        {
            var key = Convert.ToHexString(c.ChunkID!);
            var lazy = recent.GetOrAdd(key, _ =>
            {
                order.Enqueue(key);
                while (order.Count > Recent && order.TryDequeue(out var old) && old != id) recent.TryRemove(old, out var _);
                return new Lazy<Task<byte[]>>(() => steam.Chunk(depot, c));
            });
            _ = lazy.Value;
            return lazy;
        }
    }
}

// A seekable read-only view of one depot file; reads fetch the chunks under them (and read ahead a few).
sealed class DepotStream : Stream
{
    readonly DepotFiles owner;
    readonly DepotManifest.ChunkData[] chunks;
    readonly long length;
    long position;
    int lastChunk = -1;

    public DepotStream(DepotFiles owner, DepotManifest.FileData file)
    {
        this.owner = owner;
        chunks = [.. file.Chunks.OrderBy(c => c.Offset)];
        length = (long)file.TotalSize;
    }

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => length;
    public override long Position { get => position; set => position = value; }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        var total = 0;
        while (buffer.Length > 0 && position < length)
        {
            var i = Find(position);
            if (i < 0) break;
            var c = chunks[i];
            // Read ahead only once the file is being read in order: a table read needs just its first chunk.
            var data = owner.Chunk(c, i == lastChunk + 1 && lastChunk >= 0 ? chunks.Skip(i + 1).Take(12) : []);
            lastChunk = i;
            var within = (int)(position - (long)c.Offset);
            var n = Math.Min(buffer.Length, data.Length - within);
            if (n <= 0) break;
            data.AsSpan(within, n).CopyTo(buffer);
            buffer = buffer[n..];
            position += n;
            total += n;
        }
        return total;
    }

    int Find(long at)
    {
        int lo = 0, hi = chunks.Length - 1, found = -1;
        while (lo <= hi)
        {
            var mid = (lo + hi) / 2;
            if ((long)chunks[mid].Offset <= at) { found = mid; lo = mid + 1; } else hi = mid - 1;
        }
        return found >= 0 && at < (long)chunks[found].Offset + chunks[found].UncompressedLength ? found : -1;
    }

    public override long Seek(long offset, SeekOrigin origin) => position = origin switch
    {
        SeekOrigin.Begin => offset,
        SeekOrigin.Current => position + offset,
        _ => length + offset,
    };

    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

// The chunks of builds installed on this PC, by their depot manifests: each chunk ID (the SHA-1 of its bytes) with the
// installed file and offset that hold it. A chunk an older or newer build shares with an installed one is read from the
// installed file and checked by SHA-1 (a file changed since, by a patch or a mod, simply fails the check and the chunk is
// downloaded), the way a Steam update reuses what is on disk.
sealed class LocalChunks
{
    readonly Dictionary<string, (string Path, long Offset, int Length)> chunks = new(StringComparer.Ordinal);
    public long Read, Failed;

    public int Count => chunks.Count;

    // install: the folder the depot's files are in (the one holding data/).
    public void Add(DepotManifest manifest, string install)
    {
        foreach (var f in manifest.Files ?? [])
        {
            var path = Path.Combine(install, f.FileName.Replace('\\', '/'));
            if ((f.Flags & EDepotFileFlag.Directory) != 0 || !File.Exists(path) || new FileInfo(path).Length != (long)f.TotalSize) continue;
            foreach (var c in f.Chunks) chunks.TryAdd(Convert.ToHexString(c.ChunkID!), (path, (long)c.Offset, (int)c.UncompressedLength));
        }
    }

    public bool Has(DepotManifest.ChunkData chunk) => chunks.ContainsKey(Convert.ToHexString(chunk.ChunkID!));

    public byte[]? TryRead(DepotManifest.ChunkData chunk)
    {
        if (!chunks.TryGetValue(Convert.ToHexString(chunk.ChunkID!), out var at) || at.Length != chunk.UncompressedLength) return null;
        var bytes = new byte[at.Length];
        try
        {
            using var s = File.OpenRead(at.Path);
            s.Position = at.Offset;
            s.ReadExactly(bytes);
        }
        catch (IOException) { Interlocked.Increment(ref Failed); return null; }
        if (!System.Security.Cryptography.SHA1.HashData(bytes).AsSpan().SequenceEqual(chunk.ChunkID)) { Interlocked.Increment(ref Failed); return null; }
        Interlocked.Increment(ref Read);
        return bytes;
    }
}
