using System.Globalization;

namespace HellForge.Core;

// Copies of a profile's profile.json taken before an account import or a restore replaces it, in the profile's backups
// folder; the newest ten are kept. Profile > Account sync lists them and restores one.
public static class ProfileBackups
{
    public const string Folder = "backups";
    public const int Keep = 10;
    const string ProfileFile = "profile.json";
    const string Stamp = "yyyyMMdd-HHmmss-fff";

    public sealed record Backup(string Path, DateTime SavedUtc, string Reason);

    // The backup's path, or null when the profile has no profile.json yet.
    public static string? Save(string profileFolder, string reason, DateTime nowUtc)
    {
        if (reason.Length == 0 || !reason.All(char.IsAsciiLetterLower)) throw new ArgumentException("A backup reason is lower-case letters.", nameof(reason));
        var profile = Path.Combine(profileFolder, ProfileFile);
        if (!File.Exists(profile)) return null;
        var folder = Directory.CreateDirectory(Path.Combine(profileFolder, Folder)).FullName;
        var path = Path.Combine(folder, nowUtc.ToString(Stamp, CultureInfo.InvariantCulture) + "-" + reason + ".json");
        File.Copy(profile, path, overwrite: true);
        foreach (var old in List(profileFolder).Skip(Keep)) File.Delete(old.Path);
        return path;
    }

    // Newest first; files named otherwise are not backups and are left out.
    public static IReadOnlyList<Backup> List(string profileFolder)
    {
        var folder = Path.Combine(profileFolder, Folder);
        if (!Directory.Exists(folder)) return [];
        var backups = new List<Backup>();
        foreach (var path in Directory.EnumerateFiles(folder, "*.json"))
        {
            var name = Path.GetFileNameWithoutExtension(path);
            if (name.Length <= Stamp.Length + 1 || name[Stamp.Length] != '-' ||
                !DateTime.TryParseExact(name[..Stamp.Length], Stamp, CultureInfo.InvariantCulture,
                    DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var saved)) continue;
            backups.Add(new Backup(path, saved, name[(Stamp.Length + 1)..]));
        }
        return [.. backups.OrderByDescending(b => b.SavedUtc).ThenByDescending(b => b.Path, StringComparer.Ordinal)];
    }
}
