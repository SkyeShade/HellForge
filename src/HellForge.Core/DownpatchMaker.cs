using System.Collections.Concurrent;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// Workers: how many files are diffed at once (default: half the cores, at most 4). Quality: Brotli's, 0 to 11.
public sealed record DownpatchOptions(int? Workers = null, int Quality = 5);

// Makes a downpatch file (Downpatch) on a PC that has both: Steam's HELLDIVERS 2 (any build, usually the latest) and a
// HellForge installation with the build HellForge runs. Every file of the installation's copy is checked against the
// build's list first; Steam's files with the same content are only named, the others are diffed against Steam's file of
// the same path, or carried whole when Steam has none. Each delta is applied back once and checked before it goes in.
public static class DownpatchMaker
{
    // Files carried whole: players change settings.ini in Steam's copy, and a small file costs less whole than a
    // failed patch would.
    const long SmallFile = 64 << 10;

    public static DownpatchResult Make(string game, DownpatchSource source, SupportedBuild build, string outFile,
        Action<DownpatchProgress>? progress = null, CancellationToken cancellation = default, DownpatchOptions? options = null)
    {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        options ??= new();
        game = LocalInstall.NoLinks(game, game);
        var sourceRoot = LocalInstall.NoLinks(source.Folder, source.Folder);
        outFile = LocalInstall.FullPath(outFile);
        if (source.Pin.BuildId == build.BuildId || source.Pin.ExeSha256.Equals(build.ExeSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Steam already has build {build.BuildId}, the one HellForge runs: a friend with that build needs no downpatch file.");
        if (LocalInstall.Within(outFile, game) || LocalInstall.Within(outFile, sourceRoot))
            throw new InvalidDataException("Save the downpatch file outside the game folders.");
        var targets = CheckInstallation(game, build, progress, cancellation);
        var entries = Decide(targets, new SourceFiles(sourceRoot), progress, cancellation);
        var work = entries.Where(e => e.Draft.Op != "same").ToList();
        var outFolder = Path.GetDirectoryName(outFile)!;
        Directory.CreateDirectory(outFolder);
        // the whole files, and a quarter of the patched ones' size: most of a changed file is found in Steam's
        DiskSpace.Require(Downpatch.SpaceOf, (outFolder, work.Sum(e => e.Draft.Op == "new" ? e.Draft.Size : e.Draft.Size / 4)));
        var manifest = new DownpatchManifest(Downpatch.Format, Downpatch.FormatVersion, LocalInstall.AppId,
            new(source.Pin.BuildId, source.Pin.ExeSha256, source.Pin.GameDllSha256, source.Depots),
            new(build.BuildId, build.ExeSha256, build.GameDllSha256, build.Depots), "HellForge " + AppVersion.Text, []);
        try { manifest = Write(manifest, entries, work, outFile, options, progress, cancellation); }
        catch (IOException error) when (error is not NotEnoughSpaceException && Downpatch.SpaceOf(outFolder).FreeBytes < DiskSpace.Margin)
        {
            throw new NotEnoughSpaceException($"{Downpatch.SpaceOf(outFolder).Name} ran out of space while making the downpatch file: free some space or save it on another drive.");
        }
        return new(source.Pin.BuildId, build.BuildId, entries.Count, entries.Count(e => e.Draft.Op == "same"), entries.Count(e => e.Draft.Op == "patch"),
            entries.Count(e => e.Draft.Op == "new"), 0, build.TotalBytes, new FileInfo(outFile).Length, clock.Elapsed.TotalSeconds);
    }

    // The installation's file for each of the build's, with its content (a swapped upscaler keeps its own); refused
    // unless every other file is exactly the build's.
    sealed record Target(SupportedFile File, string Path, long Size, string Sha1);

    static List<Target> CheckInstallation(string game, SupportedBuild build, Action<DownpatchProgress>? progress, CancellationToken cancellation)
    {
        long done = 0, total = build.TotalBytes;
        var targets = new List<Target>();
        foreach (var file in build.Files)
        {
            var path = Path.Combine(game, file.Path);
            if (File.Exists(path + GameSettings.OriginalSuffix)) path += GameSettings.OriginalSuffix;
            if (OperatingSystem.IsLinux()) path = LocalInstall.NoLinks(path, game);
            var info = new FileInfo(path);
            if (!info.Exists) throw new InvalidDataException($"HellForge's game copy lacks {file.Path}: run Check and repair first.");
            var sha1 = Sha1(path, count => progress?.Invoke(new("Checking HellForge's game files", done += count, total, file.Path)), cancellation);
            if (!SupportedBuild.IsReplaceable(file.Path) && (info.Length != file.Size || sha1 != file.Sha1.ToUpperInvariant()))
                throw new InvalidDataException($"HellForge's game copy has a changed {file.Path}: run Check and repair first.");
            targets.Add(new(file, path, info.Length, sha1));
        }
        return targets;
    }

    // Steam's game files by path, each hashed once, when first needed.
    sealed class SourceFiles(string root)
    {
        public readonly string Root = root;
        public readonly Dictionary<string, long> Sizes = LocalInstall.FilesNoLinks(root)
            .ToDictionary(path => Path.GetRelativePath(root, path).Replace('\\', '/'), path => new FileInfo(path).Length, StringComparer.Ordinal);
        readonly Dictionary<string, string> hashes = new(StringComparer.Ordinal);

        public string Sha1(string path, Action<int> read, CancellationToken cancellation)
        {
            if (!hashes.TryGetValue(path, out var hash))
                hashes[path] = hash = DownpatchMaker.Sha1(LocalInstall.NoLinks(System.IO.Path.Combine(Root, path), Root), read, cancellation);
            return hash;
        }
    }

    sealed record Pending(int Index, DownpatchEntry Draft, string TargetPath, string? FromPath);

    static List<Pending> Decide(List<Target> targets, SourceFiles source, Action<DownpatchProgress>? progress, CancellationToken cancellation)
    {
        long done = 0, total = targets.Sum(t => source.Sizes.GetValueOrDefault(t.File.Path));
        var entries = new List<Pending>();
        for (var i = 0; i < targets.Count; i++)
        {
            var target = targets[i];
            void Read(int count) => progress?.Invoke(new("Reading Steam's game files", done += count, total, target.File.Path));
            var path = target.File.Path;
            var draft = new DownpatchEntry(path, "new", target.Size, target.Sha1, Entry: $"files/{i:D3}.hfd");
            string? from = null;
            if (path != "data/settings.ini" && SameIn(source, target, Read, cancellation) is { } same)
            {
                draft = draft with { Op = "same", From = same, Entry = null };
                from = same;
            }
            else if (path != "data/settings.ini" && target.Size >= SmallFile && source.Sizes.ContainsKey(path))
            {
                draft = draft with { Op = "patch", From = path };
                from = path;
            }
            if (from != null) draft = draft with { FromSize = source.Sizes[from], FromSha1 = source.Sha1(from, Read, cancellation) };
            entries.Add(new(i, draft, target.Path, from == null ? null : Path.Combine(source.Root, from)));
        }
        return entries;
    }

    // Steam's file with this content: at the same path first, then any of the same size.
    static string? SameIn(SourceFiles source, Target target, Action<int> read, CancellationToken cancellation)
    {
        var path = target.File.Path;
        var candidates = source.Sizes.Where(s => s.Value == target.Size).Select(s => s.Key).OrderBy(p => p == path ? 0 : 1).ToList();
        return candidates.FirstOrDefault(p => source.Sha1(p, read, cancellation) == target.Sha1);
    }

    static string Sha1(string path, Action<int> read, CancellationToken cancellation)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        using var hash = new HashingStream(input, HashAlgorithmName.SHA1) { Passed = read };
        var buffer = new byte[1 << 20];
        while (hash.Read(buffer, 0, buffer.Length) > 0) cancellation.ThrowIfCancellationRequested();
        return hash.Hex();
    }

    sealed class NoState : IDisposable { public void Dispose() { } }

    // The zip: each delta made in a folder beside it by a few threads, moved into the zip as it is done, then the
    // README and the manifest. Only a complete file gets outFile's name; a stopped one leaves nothing behind.
    static DownpatchManifest Write(DownpatchManifest manifest, List<Pending> entries, List<Pending> work, string outFile,
        DownpatchOptions options, Action<DownpatchProgress>? progress, CancellationToken cancellation)
    {
        var partial = outFile + ".partial";
        var parts = outFile + ".parts";
        Directory.CreateDirectory(parts);
        var stored = new Dictionary<int, (long Size, string Sha256)>();
        using var ready = new BlockingCollection<(Pending Item, string Part)>();
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        long done = 0, total = work.Sum(w => w.Draft.Size);
        void Scanned(long count, string file) => progress?.Invoke(new("Making the downpatch file", Interlocked.Add(ref done, count), total, file));
        var makers = Task.Run(() =>
        {
            try
            {
                Downpatch.RunParallel(work.OrderByDescending(w => w.Draft.Size).ToList(), () => new NoState(), (_, item) =>
                {
                    var part = Path.Combine(parts, $"{item.Index:D3}.hfd");
                    MakeDelta(item, part, options.Quality, Scanned, stop.Token);
                    ready.Add((item, part), stop.Token);
                }, stop.Token, options.Workers);
            }
            finally { ready.CompleteAdding(); }
        }, stop.Token);
        try
        {
            using (var file = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            using (var zip = new ZipArchive(file, ZipArchiveMode.Create))
            {
                foreach (var (item, part) in ready.GetConsumingEnumerable(stop.Token))
                {
                    stored[item.Index] = Store(zip, item.Draft.Entry!, part);
                    File.Delete(part);
                }
                makers.GetAwaiter().GetResult();
                manifest = manifest with
                {
                    Files = [.. entries.Select(e => stored.TryGetValue(e.Index, out var s) ? e.Draft with { EntrySize = s.Size, EntrySha256 = s.Sha256 } : e.Draft)]
                };
                AddText(zip, "README.txt", Readme(manifest));
                AddText(zip, Downpatch.ManifestName, JsonSerializer.Serialize(manifest, CoreJsonCamel.Default.DownpatchManifest) + "\n");
                file.Flush(true);
            }
            File.Move(partial, outFile, overwrite: true);
            return manifest;
        }
        catch
        {
            stop.Cancel();
            try { makers.Wait(); } catch (AggregateException) { }
            File.Delete(partial);
            throw;
        }
        finally
        {
            if (Directory.Exists(parts)) Directory.Delete(parts, recursive: true);
        }
    }

    // One delta into part, then applied back to nowhere: it must make exactly the installation's file.
    static void MakeDelta(Pending item, string part, int quality, Action<long, string> scanned, CancellationToken cancellation)
    {
        long last = 0;
        using (var output = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            BinaryDelta.Create(item.FromPath, item.TargetPath, output, quality, position =>
            {
                scanned(position - last, item.Draft.Path);
                last = position;
            }, cancellation);
        using var delta = new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        using var source = item.FromPath == null ? null : File.OpenHandle(item.FromPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var check = new HashingStream(Stream.Null, HashAlgorithmName.SHA1);
        BinaryDelta.Apply(source, delta, check, cancellation);
        if (check.Bytes != item.Draft.Size || check.Hex() != item.Draft.Sha1)
            throw new InvalidDataException($"The delta for {item.Draft.Path} did not reproduce it; nothing was saved. Please report this.");
    }

    static (long Size, string Sha256) Store(ZipArchive zip, string name, string part)
    {
        var entry = zip.CreateEntry(name, CompressionLevel.NoCompression);
        using var input = new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        using var output = new HashingStream(entry.Open(), HashAlgorithmName.SHA256);
        input.CopyTo(output, 1 << 20);
        return (output.Bytes, output.Hex());
    }

    static void AddText(ZipArchive zip, string name, string text)
    {
        using var writer = new StreamWriter(zip.CreateEntry(name, CompressionLevel.Optimal).Open(), new UTF8Encoding(false));
        writer.Write(text);
    }

    static string Readme(DownpatchManifest manifest) =>
        $"""
        HellForge downpatch file
        ========================

        This file turns HELLDIVERS 2 build {manifest.Source.BuildId}, as Steam installs it, into build {manifest.Target.BuildId}, the build
        that {manifest.CreatedWith} runs. A player made it on their own PC. HellForge doesn't host these; you make one
        and share it yourself.

        To use it: install HELLDIVERS 2 in Steam (it must be build {manifest.Source.BuildId}), start HellForge setup, choose
        "Use a downpatch file" and pick this file. Setup still checks that you own the game, and every file it writes
        must match Steam's hashes for build {manifest.Target.BuildId}.

        It holds only what differs between the two builds: {manifest.Files.Count(f => f.Op == "same")} files are taken from your Steam
        install as they are, {manifest.Files.Count(f => f.Op == "patch")} are patched from it, and {manifest.Files.Count(f => f.Op == "new")} are carried whole.

        """;
}
