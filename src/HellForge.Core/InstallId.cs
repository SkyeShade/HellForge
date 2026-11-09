using System.Globalization;

namespace HellForge.Core;

// A random id for this installation (install-id.txt in the host's folder, beside launcher-settings.json), made the first
// time it is asked for and kept after. The HellForge war server (backend --war hellforge, docs/hellforge-war.md) gets it
// with each mission report, so it can count one PC's missions without knowing who plays: it is not derived from the
// Steam account, the PC or anything else.
public static class InstallId
{
    public const string FileName = "install-id.txt";

    public static Guid Get(string hostRoot)
    {
        var file = Path.Combine(hostRoot, FileName);
        try
        {
            if (File.Exists(file) && Guid.TryParseExact(File.ReadAllText(file).Trim(), "D", out var saved) && saved != Guid.Empty) return saved;
            var made = Guid.NewGuid();
            Directory.CreateDirectory(hostRoot);
            var temporary = file + ".tmp";
            File.WriteAllText(temporary, made.ToString("D", CultureInfo.InvariantCulture) + "\n");
            File.Move(temporary, file, overwrite: true);
            return made;
        }
        // A folder that cannot be written still gets an id, for this session only.
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return Guid.NewGuid(); }
    }
}
