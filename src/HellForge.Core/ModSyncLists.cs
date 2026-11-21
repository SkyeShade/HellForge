namespace HellForge.Core;

// Changes to what mod sync keeps for the player (HellForge.Cli mod-sync, as the Mods page's Sharing card makes them):
// a trust taken back with its download, the blocklist, and every download deleted. Check reads all of them and refuses
// the whole call for one that is wrong, before Apply changes anything: a caller that gets an error knows nothing moved.
public static class ModSyncLists
{
    // Untrust: the trusted mod (id and hash) to take back, or null. DeleteDownloaded: every mod downloaded from hosts.
    public sealed record Changes(string ModsFolder, (string Id, string Hash)? Untrust, string? Block, string? Unblock, bool DeleteDownloaded)
    {
        public bool Any => Untrust != null || Block != null || Unblock != null || DeleteDownloaded;
    }

    // The options as given (null when absent); deleteDownloaded is "yes" or "no". sessionRunning is asked only when a
    // change needs the session to have ended (untrusting and deleting, as the Mods page disables them).
    // ArgumentException: an option is wrong. InvalidDataException: a session runs.
    public static Changes Check(string modsFolder, string? untrust, string? hash, string? block, string? unblock, string? deleteDownloaded, Func<bool> sessionRunning)
    {
        var delete = deleteDownloaded switch
        {
            null or "no" => false,
            "yes" => true,
            _ => throw new ArgumentException("--delete-downloaded takes yes or no.")
        };
        var trust = CheckUntrust(modsFolder, untrust, hash);
        CheckBlocks(block, unblock);
        if ((trust != null || delete) && sessionRunning())
            throw new InvalidDataException("A HellForge session is running: change mods once it has ended.");
        return new(modsFolder, trust, block, unblock, delete);
    }

    static (string Id, string Hash)? CheckUntrust(string modsFolder, string? untrust, string? hash)
    {
        if (untrust == null) return hash == null ? null : throw new ArgumentException("--hash goes with --untrust.");
        if (hash == null) throw new ArgumentException("--untrust needs the trusted mod's --hash (mod-sync lists it).");
        if (!ModSettings.Read(modsFolder).Trusted.Any(t => t.Id == untrust && t.Hash == hash))
            throw new ArgumentException($"{untrust} with that hash is not trusted.");
        return (untrust, hash);
    }

    static void CheckBlocks(string? block, string? unblock)
    {
        foreach (var id in new[] { block, unblock })
            if (id != null && !ModCatalog.IsModId(id)) throw new ArgumentException($"{id} is not a mod id.");
        if (block != null && block == unblock) throw new ArgumentException($"--block and --unblock both name {block}.");
    }

    public static void Apply(Changes changes)
    {
        if (changes.Untrust is var (id, hash))
        {
            ModSettings.Untrust(changes.ModsFolder, id, hash);
            SyncedStore.Forget(changes.ModsFolder, id, hash);
        }
        if (changes.Block != null) ModSettings.SetBlocked(changes.ModsFolder, changes.Block, true);
        if (changes.Unblock != null) ModSettings.SetBlocked(changes.ModsFolder, changes.Unblock, false);
        if (changes.DeleteDownloaded && SyncedStore.Root(changes.ModsFolder) is var store && Directory.Exists(store))
            Directory.Delete(store, recursive: true);
    }
}
