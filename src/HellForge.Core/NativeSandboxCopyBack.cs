namespace HellForge.Core;

public sealed record SandboxCopyBackReceipt(int Files, long Bytes, int Skipped, string Staging);

// The native half of the Linux copy-back. It runs in HellForge.Cli, where OperatingSystem.IsLinux() is true, so every
// game-written entry is opened through SandboxFiles' openat2 path. The Wine runner only merges this trusted staging copy.
public static class NativeSandboxCopyBack
{
    public static string Staging(string run) => Path.Combine(run, "copy-back");

    public static SandboxCopyBackReceipt Run(InstallLayout install, string run, string profile)
    {
        if (!OperatingSystem.IsLinux()) throw new InvalidDataException("The Linux sandbox copy-back needs native Linux.");
        if (string.IsNullOrWhiteSpace(profile) || !GameSettings.ProfileName().IsMatch(profile))
            throw new InvalidDataException("The sandbox copy-back has no valid profile name.");
        if (string.IsNullOrWhiteSpace(run)) throw new InvalidDataException("The sandbox copy-back has no run folder.");
        run = Path.TrimEndingDirectorySeparator(Path.GetFullPath(run));
        if (!SandboxProtocol.Within(install.Sessions, run) || !Directory.Exists(run) ||
            Path.GetFileName(run).Length != 32 || Path.GetFileName(run).Any(c => !char.IsAsciiHexDigit(c)))
            throw new InvalidDataException("The sandbox copy-back's run folder is not a session of this installation.");
        var staging = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Staging(run)));
        if (!SandboxProtocol.Within(run, staging) || !string.Equals(staging, Path.Combine(run, "copy-back"), StringComparison.Ordinal))
            throw new InvalidDataException("The sandbox copy-back's staging folder is not <run>/copy-back.");

        LocalInstall.NoLinks(run, install.Sessions);
        if (Directory.Exists(staging))
        {
            LocalInstall.NoLinks(staging, run);
            Directory.Delete(staging, recursive: true);
        }
        Directory.CreateDirectory(staging);
        int files = 0, skipped = 0;
        long bytes = 0;
        void Copy(string fromRoot, string toRoot, IReadOnlyList<string> parts, long maxBytes)
        {
            if (!Directory.Exists(fromRoot)) return;
            var (count, length, left) = SandboxFiles.CopyTree(fromRoot, toRoot, parts, maxBytes, SandboxFolders.MaxFiles);
            files += count; bytes += length; skipped += left;
        }

        var userData = Path.Combine(run, "game", "userdata");
        var game = Path.Combine(run, "game");
        Copy(userData, Path.Combine(staging, "userdata"), SandboxFolders.UserDataParts(profile).ToList(), SandboxFolders.MaxBytes);
        Copy(game, Path.Combine(staging, "game"), ["environment", "sandbox-stats", "domain"], 512L << 20);
        return new(files, bytes, skipped, staging);
    }
}
