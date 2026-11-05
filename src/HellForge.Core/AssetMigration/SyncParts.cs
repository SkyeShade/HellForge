using System.Security.Cryptography;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// Mod sync of game patch files without sending the game's bytes over the link (docs/multiplayer-mod-sync.md "Mod sync from your
// own game"). The host lists every resource part of a mod's archives: where it lies in which file, its key and its
// SHA-256 (Of: read from the archives' tables, then each part hashed; no game index). The joiner writes each part its own
// game has with those bytes (the same key in any build here, or any key the cached game index knows those bytes under),
// and downloads only the rest (Fill: the ranges still missing). The host's list is untrusted: a part is used only when
// the joiner's own bytes have its SHA-256, and every file is still checked against the host's file list after.
public static class SyncParts
{
    public sealed record Part(string File, long Start, long Length, ResourceKey Key, PartKind Kind, string Sha256);

    // The most parts one mod's list may hold, and the largest part taken from the game (a game part is far smaller).
    public const int MaxParts = 1 << 20;
    public const long MaxPartBytes = 1L << 30;

    // The parts of the mod's archives among files (relative paths with '/', as its file list has them): each part whose
    // file is one of them, in file order.
    public static List<Part> Of(string modFolder, IReadOnlyCollection<string> files)
    {
        var listed = files.ToHashSet(StringComparer.Ordinal);
        var mod = ModFiles.InFolder(modFolder, files.Where(f => ModCatalog.PatchFile().IsMatch(Path.GetFileName(f))));
        var parts = new List<Part>();
        var open = new Dictionary<string, FileStream>(StringComparer.Ordinal);
        try
        {
            foreach (var range in Migrator.Layout(mod).Where(r => listed.Contains(r.File)).OrderBy(r => r.File, StringComparer.Ordinal).ThenBy(r => r.Start))
            {
                if (!open.TryGetValue(range.File, out var stream))
                    open[range.File] = stream = new FileStream(Path.Combine(modFolder, range.File), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (range.Length > MaxPartBytes || range.Start < 0 || range.Start + range.Length > stream.Length) continue;
                var bytes = new byte[range.Length];
                stream.Seek(range.Start, SeekOrigin.Begin);
                stream.ReadExactly(bytes);
                parts.Add(new(range.File, range.Start, range.Length, range.Key, range.Part, Convert.ToHexString(SHA256.HashData(bytes))));
                if (parts.Count >= MaxParts) break;
            }
        }
        finally
        {
            foreach (var stream in open.Values) stream.Dispose();
        }
        return parts;
    }

    // Why a host's list cannot be used (parts outside their file, or overlapping), or null.
    public static string? Check(IReadOnlyList<Part> parts, IReadOnlyDictionary<string, long> sizes)
    {
        if (parts.Count > MaxParts) return "it lists too many parts";
        foreach (var file in parts.GroupBy(p => p.File, StringComparer.Ordinal))
        {
            if (!sizes.TryGetValue(file.Key, out var size)) return $"it lists a part of {file.Key}, which is not one of the mod's files";
            long end = 0;
            foreach (var part in file.OrderBy(p => p.Start))
            {
                if (part.Start < end || part.Length is < 1 or > MaxPartBytes || part.Start + part.Length > size)
                    return $"it lists a part of {file.Key} outside the file or over another";
                end = part.Start + part.Length;
            }
        }
        return null;
    }

    // What the joiner's builds supplied, and what is still to download: per file with any part found here, the ranges it
    // still needs (files with none found are not listed: they download whole).
    public sealed record Filled(Dictionary<string, List<(long Start, long Length)>> Missing, long LocalBytes, int LocalParts, int Parts);

    // Writes into each file (target: its path in the download; created at its full size on the first part found) every
    // part one of the given builds, or the index's places, has with the listed SHA-256. progress: bytes written.
    public static Filled Fill(IReadOnlyList<Part> parts, IReadOnlyDictionary<string, long> sizes, Func<string, string> target,
        IReadOnlyList<IGameResources> games, SourceIndex? index, Action<long>? progress = null, CancellationToken cancel = default)
    {
        var found = new Dictionary<string, List<Part>>(StringComparer.Ordinal);
        var outputs = new Dictionary<string, FileStream>(StringComparer.Ordinal);
        long local = 0;
        var count = 0;
        try
        {
            // each distinct part's bytes are read once, then written wherever the mod has them
            foreach (var same in parts.GroupBy(p => (p.Sha256, p.Length)))
            {
                cancel.ThrowIfCancellationRequested();
                if (Find(same.ToList(), games, index) is not { } bytes) continue;
                foreach (var part in same)
                {
                    var output = Output(outputs, part.File, sizes, target);
                    output.Seek(part.Start, SeekOrigin.Begin);
                    output.Write(bytes);
                    (found.TryGetValue(part.File, out var list) ? list : found[part.File] = []).Add(part);
                    local += bytes.Length;
                    count++;
                    progress?.Invoke(bytes.Length);
                }
            }
        }
        finally
        {
            foreach (var output in outputs.Values) output.Dispose();
        }
        var missing = found.ToDictionary(f => f.Key, f => Gaps(f.Value, sizes[f.Key]), StringComparer.Ordinal);
        return new(missing, local, count, parts.Count);
    }

    static FileStream Output(Dictionary<string, FileStream> outputs, string file, IReadOnlyDictionary<string, long> sizes, Func<string, string> target)
    {
        if (outputs.TryGetValue(file, out var output)) return output;
        var path = target(file);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        output.SetLength(sizes[file]);
        return outputs[file] = output;
    }

    // The bytes of these parts (one SHA-256) from this PC: the mod's own key in every build, then each place the index
    // knows those bytes at; null when none has them.
    static byte[]? Find(IReadOnlyList<Part> same, IReadOnlyList<IGameResources> games, SourceIndex? index)
    {
        if (same[0].Length > int.MaxValue) return null;
        var source = new RecipeSource { Sha256 = same[0].Sha256, Size = (int)same[0].Length };
        foreach (var part in same.DistinctBy(p => (p.Key, p.Kind)))
            source.Places.Add(Place(part.Key, part.Kind, null, null));
        if (index?.Find(Convert.FromHexString(same[0].Sha256)) is { } content)
            foreach (var place in index.Contents[content].Places)
                source.Places.Add(Place(place.Key, place.Part, index.Builds[place.Build], index.Archives[place.Archive]));
        return RecipeRebuilder.Find(source, games);
    }

    static RecipePlace Place(ResourceKey key, PartKind kind, string? build, string? archive) =>
        new() { Build = build ?? "", Type = key.Type.ToString("x16"), Name = key.Name.ToString("x16"), Part = kind.ToString().ToLowerInvariant(), Archive = archive };

    // The ranges of a file of size bytes that the found parts do not cover, in order.
    static List<(long Start, long Length)> Gaps(List<Part> found, long size)
    {
        var gaps = new List<(long, long)>();
        long at = 0;
        foreach (var part in found.OrderBy(p => p.Start))
        {
            if (part.Start > at) gaps.Add((at, part.Start - at));
            at = Math.Max(at, part.Start + part.Length);
        }
        if (at < size) gaps.Add((at, size - at));
        return gaps;
    }
}
