using System.Collections.Concurrent;
using System.Globalization;
using System.Security.Cryptography;
using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.LocalBackend.Multiplayer;

// Mod sync from the joiner's own game (SyncParts; docs/multiplayer-mod-sync.md "Mod sync from your own game"). The host lists the
// resource parts of its mods' game patch files (mods.parts) and sends a recipe mod's rebuilt files to a joiner whose game
// lacks what the recipe takes; the joiner writes what its own game has and downloads only the rest, every file still
// checked against the host's list.
public sealed partial class ModSync
{
    // ---- Host ---------------------------------------------------------------------------------------------------------

    // Each shared mod's part list (SyncParts.Of), made once per mod and content: reading the mod's archives' tables and
    // hashing its parts takes seconds for the largest mods, on the one worker.
    readonly ConcurrentDictionary<string, Lazy<List<SyncParts.Part>>> hostParts = new(StringComparer.Ordinal);

    List<SyncParts.Part> HostParts(ModSetEntry mod)
    {
        var key = mod.Id + "@" + mod.Hash;
        var parts = hostParts.GetOrAdd(key, _ => new(() => SyncParts.Of(mod.Folder!, [.. mod.Files.Select(f => f.Path)])));
        try { return parts.Value; }
        catch
        {
            hostParts.TryRemove(key, out _);
            throw;
        }
    }

    // A recipe mod's game patch files as this host rebuilt them (ModAssets), by path, when they are ready; none for any
    // other mod. Read once per mod and content.
    readonly ConcurrentDictionary<string, Dictionary<string, ModFile>> hostOutputs = new(StringComparer.Ordinal);

    Dictionary<string, ModFile> HostOutputs(ModSetEntry mod)
    {
        if (mod.Folder == null || mod.Class == ModClass.Bundled) return [];
        var key = mod.Id + "@" + mod.Hash;
        if (hostOutputs.TryGetValue(key, out var known)) return known;
        try
        {
            // not ready yet (the Mods page is still rebuilding it): asked again next time
            if (!ModAssets.HasRecipe(mod.Folder) || ModAssets.Read(mod.Folder) is not { Ready: true } info) return [];
            var outputs = info.Outputs.ToHashSet(StringComparer.Ordinal);
            return hostOutputs[key] = Recipe.LoadRecipe(mod.Folder).Files.Where(f => outputs.Contains(f.Path))
                .ToDictionary(f => f.Path, f => new ModFile(f.Path, f.Size, f.Sha256.ToUpperInvariant()), StringComparer.Ordinal);
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException) { return []; }
    }

    // The part lists a joiner offered this set will ask for, made in the background (a failure is made again on asking).
    void WarmParts(ModSet? set)
    {
        var mods = set?.Mods.Where(m => m.Tier == CoopTier.Shared && m.Folder != null && m.Class != ModClass.Bundled &&
            m.Files.Any(f => ModCatalog.PatchFile().IsMatch(Path.GetFileName(f.Path))) && !hostParts.ContainsKey(m.Id + "@" + m.Hash)).ToList() ?? [];
        if (mods.Count == 0) return;
        _ = Task.Run(() =>
        {
            foreach (var mod in mods)
                try { HostParts(mod); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { log($"Mod sync: {mod.Id}'s part list could not be made: {error.Message}"); }
        });
    }

    // mods.parts: a page of the mod's part list.
    Frame PartPage(Frame frame, ModSetEntry mod)
    {
        var parts = HostParts(mod);
        if (!int.TryParse(frame.Text(3), NumberStyles.None, CultureInfo.InvariantCulture, out var start) || start > parts.Count)
            return Refused(frame, "not a page of the mod's part list");
        var list = new Frame("mods.partlist").Add(frame.Text(0)).Add("ok").Add(parts.Count).Add(start);
        foreach (var part in parts.Skip(start).Take(PageParts))
            list.Add(part.File).Add(part.Start).Add(part.Length).Add(part.Key.Type.ToString("x16")).Add(part.Key.Name.ToString("x16"))
                .Add(part.Kind.ToString().ToLowerInvariant()).Add(part.Sha256);
        return list;
    }

    // mods.files: a page of the mod's file list.
    static Frame FilePage(Frame frame, ModSetEntry mod)
    {
        if (!int.TryParse(frame.Text(3), NumberStyles.None, CultureInfo.InvariantCulture, out var start) || start >= mod.Files.Count)
            return Refused(frame, "not a page of the mod's file list");
        var page = mod.Files.Skip(start).Take(PageFiles).ToList();
        var list = new Frame("mods.filelist").Add(frame.Text(0)).Add("ok").Add(mod.Files.Count).Add(start);
        foreach (var file in page) list.Add(file.Path).Add(file.Size).Add(file.Sha256);
        return list;
    }

    // ---- Joiner -------------------------------------------------------------------------------------------------------

    // The files after the first (the manifest, stats and spec): from a host that lists parts, its game patch files'
    // parts this PC's game has are written from here while the other files download, and only what is left of them is
    // downloaded; a file that does not then check out downloads again whole. Otherwise every file downloads whole.
    async Task DownloadRestAsync(Offer current, ModSetEntry mod, List<ModFile> rest, string partial, DownloadWindow window, CancellationToken token)
    {
        var parts = current.Parts ? await PartListAsync(current.Host, mod, rest, token) : null;
        if (parts is not { Count: > 0 })
        {
            await DownloadFilesAsync(current, mod, rest, partial, window, token);
            return;
        }
        var withParts = parts.Select(p => p.File).ToHashSet(StringComparer.Ordinal);
        var patches = rest.Where(f => withParts.Contains(f.Path)).ToList();
        using var fillStop = CancellationTokenSource.CreateLinkedTokenSource(token);
        var filling = Task.Run(() => FillLocal(current, mod, patches, parts, partial, fillStop.Token), CancellationToken.None);
        try { await DownloadFilesAsync(current, mod, [.. rest.Where(f => !withParts.Contains(f.Path))], partial, window, token); }
        catch
        {
            fillStop.Cancel();
            try { await filling; }
            catch (OperationCanceledException) { }
            throw;
        }
        var filled = await filling;
        await DownloadFilesAsync(current, mod, [.. patches.Where(f => !filled.ContainsKey(f.Path))], partial, window, token);
        var wrong = await DownloadRangesAsync(current, mod, [.. patches.Where(f => filled.ContainsKey(f.Path))], filled, partial, window, token);
        if (wrong.Count == 0) return;
        log($"Mod sync: {mod.Id}: {wrong.Count} files made from the host's part list did not check out; they download whole.");
        await DownloadFilesAsync(current, mod, wrong, partial, window, token);
    }

    // The parts of files this PC's game has, written into the download (SyncParts.Fill): by path, the ranges each file it
    // began still needs. Empty when this PC has no game here or reading it failed (what it wrote is deleted: those files
    // download whole).
    Dictionary<string, List<(long Start, long Length)>> FillLocal(Offer current, ModSetEntry mod, List<ModFile> files, List<SyncParts.Part> parts, string partial,
        CancellationToken token)
    {
        IReadOnlyList<IGameResources> games = [];
        try
        {
            games = LocalGames();
            if (games.Count == 0) return [];
            var index = LocalIndex();
            var sizes = files.ToDictionary(f => f.Path, f => f.Size, StringComparer.Ordinal);
            var filled = SyncParts.Fill(parts, sizes, path => SyncedStore.Target(partial, path), games, index, bytes => Progress(current, (int)bytes), token);
            var left = filled.Missing.Values.Sum(m => m.Sum(r => r.Length)) + files.Where(f => !filled.Missing.ContainsKey(f.Path)).Sum(f => f.Size);
            log($"Mod sync: {mod.Id}: {Megabytes(filled.LocalBytes)} ({filled.LocalParts} of {filled.Parts} parts) written from this PC's game" +
                (index == null ? " (no game index here: only resources under their own names)" : "") + $"; {Megabytes(left)} of its game files to download.");
            // without the game index only resources under their own names are found: a large rest says how to find more
            var hint = index == null && left >= IndexHintBytes ? " Index your game on HellForge's Mods page (MOD SYNC) to take more from it next time." : "";
            if (filled.LocalBytes > 0 || hint.Length > 0)
                Update(current, "downloading", $"{Megabytes(filled.LocalBytes)} of {mod.Name} came from your own game; downloading the rest ({Megabytes(left)}) from {current.HostName}...{hint}");
            return filled.Missing;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or EndOfStreamException)
        {
            log($"Mod sync: {mod.Id}: this PC's game could not be read ({error.Message}); its game files download whole.");
            foreach (var file in files)
                try { File.Delete(SyncedStore.Target(partial, file.Path)); }
                catch (Exception gone) when (gone is IOException or UnauthorizedAccessException) { }
            return [];
        }
        finally
        {
            foreach (var game in games) (game as IDisposable)?.Dispose();
        }
    }

    // A rest this large, downloaded without the game index, brings the hint to make one.
    public const long IndexHintBytes = 64L << 20;

    // The builds of the game on this PC (HellForge's game copy, Steam's install), opened, and their index when one was made
    // (AssetKit.CachedIndex; mod sync never makes one); a test sets its own.
    public Func<IReadOnlyList<IGameResources>> LocalGames { get; set; } = () => [];
    public Func<SourceIndex?> LocalIndex { get; set; } = () => null;

    InstallLayout? Install()
    {
        var root = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(modsFolder)));
        return root != null && new InstallLayout(root) is { Exists: true } found ? found : null;
    }

    List<IGameResources> OwnGames()
    {
        var games = new List<IGameResources>();
        foreach (var build in GameBuilds.Local(Install()))
            try { games.Add(GameBuilds.Open(build)); }
            catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException) { log($"Mod sync: {build.What} could not be opened: {error.Message}"); }
        return games;
    }

    SourceIndex? OwnIndex()
    {
        var install = Install();
        return AssetKit.CachedIndex(GameBuilds.Local(install), AssetKit.CacheFolder(install));
    }

    // A recipe mod (ModAssets) from a host that sends its rebuilt files: its game patch files are made here as far as this
    // PC's game has the parts the recipe takes, and what it lacks (a build this PC does not have, as after a game update)
    // is downloaded from the host's own rebuilt files; SyncedStore.Install then finds them made. From an older host, or
    // when a recipe output would land on a file the mod ships, the rebuild is left to Install, which says what is missing.
    async Task RecipeOutputsAsync(Offer current, ModSetEntry mod, string partial, DownloadWindow window, CancellationToken token)
    {
        if (!current.Parts || !ModAssets.HasRecipe(partial)) return;
        var outputs = ModAssets.Read(partial).Outputs.ToHashSet(StringComparer.Ordinal);
        var recipe = Recipe.LoadRecipe(partial);
        var files = recipe.Files.Where(f => outputs.Contains(f.Path)).Select(f => new ModFile(f.Path, f.Size, f.Sha256.ToUpperInvariant())).ToList();
        if (files.Count != recipe.Files.Count || files.Any(f => File.Exists(SyncedStore.Target(partial, f.Path)))) return;
        var gaps = await Task.Run(() => RecipeHere(recipe, partial), token);
        var missing = gaps.Values.Sum(g => g.Sum(r => r.Length));
        if (missing > 0)
        {
            log($"Mod sync: {mod.Id}: this PC's game lacks {Megabytes(missing)} of what its recipe takes; that comes from the host's rebuilt files.");
            Update(current, "downloading", $"Your game lacks some of what {mod.Name} takes from the game ({Megabytes(missing)}); downloading that from {current.HostName}...");
        }
        var wrong = await DownloadRangesAsync(current, mod, files, gaps, partial, window, token);
        if (wrong.Count == 0) return;
        log($"Mod sync: {mod.Id}: {wrong.Count} files its recipe made here did not check out; they download whole from the host's rebuilt files.");
        await DownloadFilesAsync(current, mod, wrong, partial, window, token);
    }

    // The recipe's files written from this PC's game as far as it has the parts (RecipeRebuilder.WriteWhatCan): by path,
    // the ranges it lacks.
    Dictionary<string, List<(long Start, long Length)>> RecipeHere(Recipe recipe, string partial)
    {
        var games = LocalGames();
        try
        {
            using var literals = Recipe.OpenLiterals(partial);
            return RecipeRebuilder.WriteWhatCan(recipe, literals, games, file =>
            {
                var target = SyncedStore.Target(partial, file.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                return new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            });
        }
        finally
        {
            foreach (var game in games) (game as IDisposable)?.Dispose();
        }
    }

    // The host's part list of a mod (mods.parts), page by page, checked against its file list (SyncParts.Check); null
    // when it has no game patch files or the list cannot be used (its files then download whole).
    async Task<List<SyncParts.Part>?> PartListAsync(ulong host, ModSetEntry mod, List<ModFile> files, CancellationToken token)
    {
        if (!files.Any(f => ModCatalog.PatchFile().IsMatch(Path.GetFileName(f.Path)))) return null;
        var sizes = files.ToDictionary(f => f.Path, f => f.Size, StringComparer.Ordinal);
        try
        {
            var parts = new List<SyncParts.Part>();
            int? total = null;
            while (total == null || parts.Count < total)
            {
                var reply = await RequestAsync(host, new Frame("mods.parts").Add(mod.Id).Add(mod.Hash).Add(parts.Count), token);
                if (reply.Text(1) != "ok") throw new InvalidDataException($"the host refused ({(reply.Count > 2 ? reply.Text(2) : "no reason")})");
                total ??= reply.Int(2);
                var count = (reply.Count - 4) / 7;
                if (total is < 0 or > SyncParts.MaxParts || reply.Int(2) != total || reply.Int(3) != parts.Count || (reply.Count - 4) % 7 != 0 ||
                    count > PageParts || parts.Count + count > total || count == 0 && parts.Count < total)
                    throw new InvalidDataException("it is malformed");
                for (var i = 0; i < count; i++) parts.Add(ListedPart(reply, 4 + 7 * i, sizes));
            }
            return SyncParts.Check(parts, sizes) is { } why ? throw new InvalidDataException(why) : parts;
        }
        catch (Exception error) when (error is InvalidDataException or TimeoutException)
        {
            log($"Mod sync: {mod.Id}: the host's part list could not be used ({error.Message}); its files download whole.");
            return null;
        }
    }

    // The part a page lists at field at: one of the listed files, numbers, a key, a part kind, a SHA-256.
    static SyncParts.Part ListedPart(Frame reply, int at, IReadOnlyDictionary<string, long> sizes)
    {
        var path = reply.Text(at);
        if (!sizes.ContainsKey(path) ||
            !long.TryParse(reply.Text(at + 1), NumberStyles.None, CultureInfo.InvariantCulture, out var start) ||
            !long.TryParse(reply.Text(at + 2), NumberStyles.None, CultureInfo.InvariantCulture, out var length) ||
            !ulong.TryParse(reply.Text(at + 3), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var type) ||
            !ulong.TryParse(reply.Text(at + 4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var name) ||
            reply.Text(at + 5) is not ("main" or "gpu" or "stream") || reply.Text(at + 6) is not { Length: 64 } sha || !sha.All(char.IsAsciiHexDigitUpper))
            throw new InvalidDataException("it lists a part that cannot be taken");
        return new(path, start, length, new(type, name), Enum.Parse<PartKind>(reply.Text(at + 5), ignoreCase: true), sha);
    }

    // The ranges of files a local fill left (missing, by path), written over the files it began; then each file is checked
    // whole. The files whose bytes are not the ones listed (a host whose part list does not match its files, deleted
    // here), or none.
    async Task<List<ModFile>> DownloadRangesAsync(Offer current, ModSetEntry mod, IReadOnlyList<ModFile> files,
        IReadOnlyDictionary<string, List<(long Start, long Length)>> missing, string partial, DownloadWindow window, CancellationToken token)
    {
        var outputs = new Dictionary<int, FileStream>();
        IEnumerable<(int, long, int)> Pieces()
        {
            for (var file = 0; file < files.Count; file++)
                foreach (var (start, length) in missing[files[file].Path])
                    for (var offset = start; offset < start + length; offset += ChunkBytes)
                        yield return (file, offset, (int)Math.Min(ChunkBytes, start + length - offset));
        }
        try
        {
            await PumpAsync(current, mod, files, Pieces(), window, async (file, offset, bytes) =>
            {
                if (!outputs.TryGetValue(file, out var output))
                    outputs[file] = output = new FileStream(SyncedStore.Target(partial, files[file].Path), FileMode.Open, FileAccess.Write, FileShare.None);
                output.Seek(offset, SeekOrigin.Begin);
                await output.WriteAsync(bytes, token);
            }, token);
        }
        finally
        {
            foreach (var output in outputs.Values) await output.DisposeAsync();
        }
        var wrong = new List<ModFile>();
        foreach (var file in files)
        {
            var target = SyncedStore.Target(partial, file.Path);
            string sha;
            await using (var input = new FileStream(target, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan))
                sha = Convert.ToHexString(await SHA256.HashDataAsync(input, token));
            if (sha == file.Sha256) continue;
            File.Delete(target);
            wrong.Add(file);
        }
        return wrong;
    }
}
