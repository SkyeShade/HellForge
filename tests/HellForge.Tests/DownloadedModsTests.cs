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

        // #391's review: mod-sync applied its options one by one, so a call refused for a later option had already
        // untrusted, deleted or blocked for an earlier one.
        test("mod-sync checks every option and the session before it changes anything", () =>
        {
            var mods = Path.Combine(root, "mod-sync-lists", "mods");
            var hash = new string('b', 64);
            var stored = Directory.CreateDirectory(Path.Combine(SyncedStore.Root(mods), "test.shared@" + hash[..16])).FullName;
            File.WriteAllBytes(Path.Combine(stored, "a.bin"), new byte[300]);
            ModSettings.SetTrusted(mods, new TrustedMod("test.shared", hash, "Shared", "1.0.0", "A host (110000100000001)", "2026-11-13T00:00:00Z"));
            ModSettings.SetBlocked(mods, "test.old", true);
            var file = Directory.GetFiles(mods, "*.json").Single();
            var before = File.ReadAllText(file);
            var asked = 0;
            bool Running() { asked++; return true; }
            bool Ended() { asked++; return false; }

            void Refused<T>(string why, string? untrust, string? withHash, string? block, string? unblock, string? delete, Func<bool> session) where T : Exception
            {
                var refused = false;
                try { ModSyncLists.Apply(ModSyncLists.Check(mods, untrust, withHash, block, unblock, delete, session)); }
                catch (T) { refused = true; }
                check(refused, "Not refused: " + why);
                check(File.ReadAllText(file) == before && SyncedStore.Downloaded(mods) == (1, 300), "Refused, but something changed: " + why);
            }

            // The reproduced case: --block x --delete-downloaded maybe blocked x, then failed.
            Refused<ArgumentException>("a bad --delete-downloaded after --block", null, null, "test.partial", null, "maybe", Ended);
            Refused<ArgumentException>("a bad --block after --untrust", "test.shared", hash, "bad id", null, null, Ended);
            Refused<ArgumentException>("a bad --unblock after --block", null, null, "test.partial", "bad id", null, Ended);
            Refused<ArgumentException>("--untrust without --hash, with --block", "test.shared", null, "test.partial", null, null, Ended);
            Refused<ArgumentException>("--hash without --untrust, with --block", null, hash, "test.partial", null, null, Ended);
            Refused<ArgumentException>("an untrusted hash, with --block and --delete-downloaded", "test.shared", new string('c', 64), "test.partial", null, "yes", Ended);
            Refused<ArgumentException>("--block and --unblock of one mod", null, null, "test.partial", "test.partial", null, Ended);
            check(asked == 0, "A call with a wrong option asked whether a session runs.");
            Refused<InvalidDataException>("--block with --delete-downloaded yes during a session", null, null, "test.partial", null, "yes", Running);
            Refused<InvalidDataException>("--untrust with --unblock during a session", "test.shared", hash, null, "test.old", null, Running);

            // Blocking and unblocking need no ended session, and do not ask for one.
            asked = 0;
            ModSyncLists.Apply(ModSyncLists.Check(mods, null, null, "test.partial", "test.old", "no", Running));
            var state = ModSettings.Read(mods);
            check(asked == 0 && state.Blocked.SetEquals(["test.partial"]) && state.Trusts("test.shared", hash) && SyncedStore.Downloaded(mods) == (1, 300),
                "Block and unblock during a session: " + string.Join(",", state.Blocked));

            // Nothing given changes nothing; everything at once is all applied.
            var none = ModSyncLists.Check(mods, null, null, null, null, null, Running);
            check(!none.Any && asked == 0, "A call with no option was read as a change.");
            Directory.CreateDirectory(Path.Combine(SyncedStore.Root(mods), "test.other@" + new string('d', 16)));
            var all = ModSyncLists.Check(mods, "test.shared", hash, "test.new", "test.partial", "yes", Ended);
            check(all.Any && asked == 1, "The session was not asked once for a change that needs it ended.");
            ModSyncLists.Apply(all);
            state = ModSettings.Read(mods);
            check(!state.Trusts("test.shared", hash) && state.Blocked.SetEquals(["test.new"]) && SyncedStore.Downloaded(mods) == (0, 0) && !Directory.Exists(SyncedStore.Root(mods)),
                "Everything at once was not all applied.");
        });
    }
}
