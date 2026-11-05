using HellForge.SteamDownload;

// The asset kit's saved Steam sign-ins (SteamLogins): HellForge's own store first, then the asset migrator's, read in
// either file's form; HellForge's written atomically for the user only, renewed in place, forgotten when Steam refuses it
// or the player signs out; the migrator's never written or deleted. No Steam here: the tokens are made up.
static class SteamLoginsTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("Saved Steam sign-ins: HellForge's first, then the migrator's, which is never changed", () =>
        {
            var folder = Path.Combine(root, "steam-logins");
            var own = Path.Combine(folder, "config", "HellForge", "steam-login.json");
            var migrator = Path.Combine(folder, "config", "hellforge-migrator", "steam-login.json");
            Directory.CreateDirectory(Path.GetDirectoryName(migrator)!);
            // the migrator writes the record's names as they are
            File.WriteAllText(migrator, """{"AccountName":"diver","RefreshToken":"migrator-token"}""");
            var migratorBefore = File.ReadAllBytes(migrator);
            var logins = new SteamLogins(own, [migrator]);

            var read = logins.Read();
            check(read is [{ AccountName: "diver", RefreshToken: "migrator-token", Own: false }] && !logins.HasOwn, "Only the migrator's at first: " + read.Count);

            logins.Save("diver", "own-token");
            check(File.Exists(own) && File.ReadAllText(own).Contains("\"refreshToken\":\"own-token\""), "HellForge's sign-in is written: " + File.ReadAllText(own));
            check(Directory.GetFiles(Path.GetDirectoryName(own)!).Length == 1, "Nothing but the file is left beside it.");
            if (!OperatingSystem.IsWindows())
            {
                check(File.GetUnixFileMode(own) == (UnixFileMode.UserRead | UnixFileMode.UserWrite), "The file is the user's only: " + File.GetUnixFileMode(own));
                check((File.GetUnixFileMode(Path.GetDirectoryName(own)!) & (UnixFileMode.GroupRead | UnixFileMode.OtherRead)) == 0, "So is its folder.");
            }
            read = logins.Read();
            check(read is [{ RefreshToken: "own-token", Own: true }, { RefreshToken: "migrator-token", Own: false }], "HellForge's comes first, then the migrator's.");

            // a renewal replaces it; the same token in both lists once
            logins.Save("diver", "migrator-token");
            check(logins.Read() is [{ RefreshToken: "migrator-token", Own: true }], "A token saved in both is tried once.");
            logins.Save("diver", "renewed-token");

            // Steam refusing the migrator's sign-in leaves its file; refusing an older own token leaves the newer one
            logins.Rejected(new("diver", "migrator-token", "the asset migrator's saved sign-in", false));
            logins.Rejected(new("diver", "own-token", "HellForge's saved sign-in", true));
            check(logins.Read().Count == 2, "Neither refusal deleted anything it should not.");
            logins.Rejected(new("diver", "renewed-token", "HellForge's saved sign-in", true));
            check(!logins.HasOwn && File.Exists(migrator), "Steam refusing HellForge's own deletes just that.");

            logins.Save("diver", "own-token");
            check(logins.Forget() && !logins.HasOwn && !logins.Forget(), "Signing out deletes HellForge's, once.");
            check(File.ReadAllBytes(migrator).SequenceEqual(migratorBefore), "The migrator's file is never written.");

            // a file that is not a sign-in is left out
            File.WriteAllText(own, "{ not json");
            File.WriteAllText(migrator, """{"AccountName":"diver"}""");
            check(logins.Read().Count == 0, "Unreadable sign-ins are left out.");
        });
    }
}
