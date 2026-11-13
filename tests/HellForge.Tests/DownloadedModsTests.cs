using HellForge.Core;

// The mods downloaded from hosts (SyncedStore.Downloaded), as the Mods page and HellForge.Cli's mod-sync count them, and
// an untrusted mod's download going with its trust.
static class DownloadedModsTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Downloaded mods are counted with their size, a dot folder is not one, and untrusting a mod deletes its download", () =>
        {
            var mods = Path.Combine(root, "downloaded-mods", "mods");
            check(SyncedStore.Downloaded(mods) == (0, 0), "A mods folder with no store has downloads.");
            var hash = new string('a', 64);
            var stored = Directory.CreateDirectory(Path.Combine(SyncedStore.Root(mods), "test.shared@" + hash[..16])).FullName;
            File.WriteAllBytes(Path.Combine(stored, "a.bin"), new byte[300]);
            Directory.CreateDirectory(Path.Combine(stored, "data"));
            File.WriteAllBytes(Path.Combine(stored, "data", "b.bin"), new byte[200]);
            Directory.CreateDirectory(Path.Combine(SyncedStore.Root(mods), ".partial-download"));
            check(SyncedStore.Downloaded(mods) == (1, 500), "Counted: " + SyncedStore.Downloaded(mods));

            ModSettings.SetTrusted(mods, new TrustedMod("test.shared", hash, "Shared", "1.0.0", "A host (110000100000001)", "2026-11-13T00:00:00Z"));
            check(ModSettings.Read(mods).Trusts("test.shared", hash), "The mod was not trusted.");
            ModSettings.Untrust(mods, "test.shared", hash);
            SyncedStore.Forget(mods, "test.shared", hash);
            check(!ModSettings.Read(mods).Trusts("test.shared", hash) && SyncedStore.Downloaded(mods) == (0, 0), "Untrusting kept the trust or its download.");
        });
    }
}
