using System.Text.Json;
using HellForge.Core;

namespace HellForge.LocalBackend;

// The launcher's account import as command lines (Profile > Account sync). Preview and import read the profile's account
// snapshot only when it is sealed on this PC for the Steam account signed in now (AccountSnapshot); restore puts a profile
// backup back. Import and restore take the profile's lease, so the profile's server must be stopped.
public static class AccountCommands
{
    // Prints the review (AccountImport.Preview) as JSON. A profile not created yet is shown as its template makes it.
    public static int Preview(string profileDir, string? template, ulong? steamAccount, TextWriter output, TextWriter error)
    {
        try
        {
            using var snapshot = OpenSnapshot(profileDir, steamAccount, error);
            if (snapshot == null) return 1;
            var path = Path.Combine(profileDir, ProfileStore.FileName);
            var profile = File.Exists(path) ? ProfileStore.Load(path) : NewProfile(template);
            output.WriteLine(AccountImport.Preview(profile, snapshot, PaidContentSet.Load()).ToJsonString());
            return 0;
        }
        catch (Exception problem) when (problem is ArgumentException or InvalidDataException or IOException or UnauthorizedAccessException)
        {
            return Fail(error, problem.Message);
        }
    }

    // Applies the snapshot with the profile's saved choices (AccountImportSettings), after backing the profile up.
    public static int Import(string profileDir, string? template, ulong? steamAccount, TextWriter output, TextWriter error)
    {
        try
        {
            using var snapshot = OpenSnapshot(profileDir, steamAccount, error);
            if (snapshot == null) return 1;
            var settings = AccountImportSettings.Load(AccountImportSettings.PathIn(profileDir));
            using var store = ProfileStore.Open(profileDir, template);
            store.Import(snapshot, settings, PaidContentSet.Load());
            output.WriteLine("Imported the account snapshot into " + store.FilePath);
            return 0;
        }
        catch (Exception problem) when (problem is ArgumentException or InvalidDataException or IOException or UnauthorizedAccessException or JsonException)
        {
            return Fail(error, problem.Message);
        }
    }

    public static int Restore(string profileDir, string backupPath, TextWriter output, TextWriter error)
    {
        try
        {
            using var store = ProfileStore.Open(profileDir);
            store.Restore(backupPath);
            output.WriteLine("Restored " + Path.GetFileName(backupPath) + " into " + store.FilePath);
            return 0;
        }
        catch (Exception problem) when (problem is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            return Fail(error, problem.Message);
        }
    }

    static AccountSnapshot? OpenSnapshot(string profileDir, ulong? steamAccount, TextWriter error)
    {
        var folder = InventorySync.SnapshotDirectoryFromProfileDir(profileDir);
        string? problem = "the profile folder is not inside an installation";
        var snapshot = folder == null ? null : AccountSnapshot.Open(folder, steamAccount, out problem);
        if (snapshot == null) Fail(error, "Cannot import: " + problem + ".");
        return snapshot;
    }

    static System.Text.Json.Nodes.JsonObject NewProfile(string? template)
    {
        using var store = ProfileStore.Open(null, template);
        return store.Current();
    }

    static int Fail(TextWriter error, string message)
    {
        error.WriteLine("HellForge local backend: " + message);
        return 1;
    }
}
