using System.Text.Json;

namespace HellForge.Core;

public sealed record CopiedFile(string RelativePath, long Length, string Sha256);
public sealed record CopyReceipt(int SchemaVersion, string State, InstallReport Source,
    IReadOnlyList<CopiedFile> Files, bool ReadyToLaunch = false);

public static class CopyPreparer
{
    public static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    // Makes an unpatched research copy. It is not a launcher or a network isolation mechanism.
    public static CopyReceipt Prepare(string manifestPath, string destination, BuildPin expected,
        Action<int, int>? progress = null)
    {
        var report = LocalInstall.Inspect(manifestPath);
        if (report.Pin != expected) throw new InvalidDataException("Build pin mismatch. Inspect and review the installed build first.");
        destination = LocalInstall.FullPath(destination);
        LocalInstall.NoLinks(destination);
        if (LocalInstall.Within(destination, report.Source) || LocalInstall.Within(report.Source, destination))
            throw new InvalidDataException("Source and destination must not overlap.");
        // Keep research copies outside this Steam library, including its update-managed folders.
        var steamApps = Path.GetDirectoryName(report.ManifestPath)!;
        if (LocalInstall.Within(destination, steamApps))
            throw new InvalidDataException("Destination must be outside the source Steam library's steamapps directory.");
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new InvalidDataException("Destination must not already exist. A failed copy must be reviewed separately.");
        var files = LocalInstall.Inventory(report.Source);
        var drive = new DriveInfo(Path.GetPathRoot(destination)!);
        if (drive.AvailableFreeSpace < files.Sum(x => x.Length) + 64L * 1024 * 1024)
            throw new IOException("Insufficient free space for a full copy and its receipt.");

        Directory.CreateDirectory(destination);
        // CreateNew prevents concurrent preparations from sharing the same destination.
        using var marker = new FileStream(Path.Combine(destination, "preparing.lock"), FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var content = Path.Combine(destination, "game");
        Directory.CreateDirectory(content);
        var copied = new List<CopiedFile>();
        foreach (var file in files)
        {
            copied.Add(CopyFile(report.Source, content, file));
            progress?.Invoke(copied.Count, files.Count);
        }
        if (!files.SequenceEqual(LocalInstall.Inventory(report.Source)) || LocalInstall.Inspect(manifestPath).Pin != expected)
            throw new IOException("Source installation changed during preparation. Copy is incomplete.");
        if (LocalInstall.Hash(Path.Combine(content, LocalInstall.Exe)) != expected.ExeSha256 ||
            LocalInstall.Hash(Path.Combine(content, LocalInstall.GameDll)) != expected.GameDllSha256)
            throw new IOException("Copied game binaries do not match the reviewed build pin.");
        var receipt = new CopyReceipt(1, "unpatched_research_copy", report, copied);
        using (var output = new FileStream(Path.Combine(destination, "copy-receipt.json"), FileMode.CreateNew))
            JsonSerializer.Serialize(output, receipt, JsonOptions);
        marker.Dispose();
        File.Delete(Path.Combine(destination, "preparing.lock"));
        return receipt;
    }

    // One file into the copy, hashed from the source and verified in the copy.
    static CopiedFile CopyFile(string sourceRoot, string content, InventoryFile file)
    {
        var source = Path.Combine(sourceRoot, file.RelativePath);
        var target = Path.Combine(content, file.RelativePath);
        LocalInstall.NoLinks(source);
        LocalInstall.NoLinks(target);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        CopiedFile copied;
        using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            if (input.Length != file.Length) throw new IOException($"Source changed during preparation: {file.RelativePath}");
            input.CopyTo(output);
            output.Flush(true);
            input.Position = 0;
            var sourceHash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(input));
            copied = new(file.RelativePath, input.Length, sourceHash);
        }
        if (LocalInstall.Hash(target) != copied.Sha256)
            throw new IOException($"Copy verification failed: {file.RelativePath}");
        return copied;
    }
}
