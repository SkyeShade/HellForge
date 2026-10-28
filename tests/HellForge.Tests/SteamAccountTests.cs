using System.Globalization;
using System.Text.Json.Nodes;
using HellForge.Core;

// The Steam account handoff (issue #88): the probe's private account id, its short lifetime, and the rule that only a
// Windows program under Wine may take the native side's environment value. Every other consumer goes through SteamAccount.
static class SteamAccountTests
{
    const uint Account = 39734273;
    const uint OtherAccount = 39734274;

    static string? RepositoryRoot()
    {
        for (var folder = Directory.GetCurrentDirectory(); folder != null; folder = Path.GetDirectoryName(folder))
            if (File.Exists(Path.Combine(folder, "linux", "hellforge-linux.sh"))) return folder;
        return null;
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The probe's private account handoff names only the 32-bit account and its check time", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "steam-account-private")).FullName;
            var path = Path.Combine(folder, "account.json");
            var checkedUtc = new DateTime(2026, 10, 28, 12, 34, 56, DateTimeKind.Utc);
            File.WriteAllText(path, new JsonObject
            {
                ["loggedOn"] = true, ["accountId"] = Account, ["checkedUtc"] = checkedUtc
            }.ToJsonString());
            var parsed = SteamAccount.ReadProbeResult(path);
            check(parsed is { AccountId: Account } result && result.CheckedUtc == checkedUtc,
                "The private result did not read back: " + parsed);
            check(SteamAccount.FromAccountId(Account) == 76561198000000001 && SteamAccount.AccountId(SteamAccount.FromAccountId(Account)) == Account,
                "The 32-bit account and its SteamID64 disagree.");

            foreach (var broken in new[]
            {
                "{not json", "[]", """{"accountId":39734273,"checkedUtc":"2026-10-28T12:34:56Z"}""",
                """{"loggedOn":false,"accountId":39734273,"checkedUtc":"2026-10-28T12:34:56Z"}""",
                """{"loggedOn":true,"accountId":0,"checkedUtc":"2026-10-28T12:34:56Z"}""",
                """{"loggedOn":true,"accountId":"39734273","checkedUtc":"2026-10-28T12:34:56Z"}""",
                """{"loggedOn":true,"accountId":39734273,"checkedUtc":123}""",
                """{"loggedOn":true,"accountId":39734273,"checkedUtc":"not a time"}"""
            })
            {
                File.WriteAllText(path, broken);
                check(SteamAccount.ReadProbeResult(path) == null, "A broken private result read as an account: " + broken);
            }
            check(SteamAccount.ReadProbeResult(Path.Combine(folder, "missing.json")) == null, "A missing private result read as an account.");
        });

        test("A session's own probe result is taken once and deleted", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "steam-session-account")).FullName;
            var path = Path.Combine(folder, "account.json");
            File.WriteAllText(path, new JsonObject
            {
                ["loggedOn"] = true, ["accountId"] = Account, ["checkedUtc"] = DateTime.UtcNow
            }.ToJsonString());
            check(SteamAccount.TakeProbeResult(path) == SteamAccount.FromAccountId(Account), "The session did not take the probe account.");
            check(!File.Exists(path), "The session left the private account file behind.");
            check(SteamAccount.TakeProbeResult(path) == null, "A missing private result returned an account.");
        });

        test("A probed account is good for five minutes, with only a little clock skew into the future", () =>
        {
            var now = new DateTime(2026, 10, 28, 12, 0, 0, DateTimeKind.Utc);
            SteamAccount.ProbeResult At(DateTime when) => new(Account, when);
            check(SteamAccount.Fresh(At(now), now), "This moment's probe was stale.");
            check(SteamAccount.Fresh(At(now.AddMinutes(-4).AddSeconds(59)), now), "A four-minute-old probe was stale.");
            check(!SteamAccount.Fresh(At(now - SteamAccount.ProbeFreshness), now), "A probe exactly five minutes old passed.");
            check(!SteamAccount.Fresh(At(now.AddDays(-1)), now), "An old probe passed.");
            check(SteamAccount.Fresh(At(now.AddSeconds(5)), now), "A five-second clock skew was refused.");
            check(!SteamAccount.Fresh(At(now.AddSeconds(6)), now), "A probe six seconds in the future passed.");
            check(!SteamAccount.Fresh(null, now), "No probe passed as fresh.");
            check(SteamAccount.ProbeFreshness == TimeSpan.FromMinutes(5), "The freshness limit changed.");
        });

        test("Only a Windows program under Wine takes HELLFORGE_STEAM_ACCOUNT; native Linux and Windows ignore it", () =>
        {
            var value = Account.ToString(CultureInfo.InvariantCulture);
            check(SteamAccount.EnvironmentVariable == "HELLFORGE_STEAM_ACCOUNT", "The handoff variable was renamed.");
            check(SteamAccount.FromWineEnvironment(value, underWine: true) == SteamAccount.FromAccountId(Account),
                "Wine refused the native side's account.");
            check(SteamAccount.FromWineEnvironment(value, underWine: false) == null, "A native program took the Wine environment value.");
            foreach (var invalid in new string?[] { null, "", "0", "-1", "abc", "4294967296", " 39734273" })
                check(SteamAccount.FromWineEnvironment(invalid, underWine: true) == null, "An invalid environment value passed: " + invalid);

            if (OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine)
            {
                var previous = Environment.GetEnvironmentVariable(SteamAccount.EnvironmentVariable);
                try
                {
                    Environment.SetEnvironmentVariable(SteamAccount.EnvironmentVariable, OtherAccount.ToString(CultureInfo.InvariantCulture));
                    var native = SteamAccount.Current();
                    check(native is not { } account || SteamAccount.AccountId(account) != OtherAccount,
                        "Native Linux took the Wine environment value.");
                }
                finally { Environment.SetEnvironmentVariable(SteamAccount.EnvironmentVariable, previous); }
            }
        });

        test("A session starts no probe in SessionRunner and adopts its own under-Wine probe account", () =>
        {
            if (RepositoryRoot() is not { } repository) { Console.WriteLine("  (skipped: no repository root above the test)"); return; }
            var sessionRunner = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Core", "SessionRunner.cs"));
            check(sessionRunner.Contains("SteamAccount.Current()", StringComparison.Ordinal) &&
                  !sessionRunner.Contains("SteamAccount.Current(new InstallLayout", StringComparison.Ordinal) &&
                  !sessionRunner.Contains("SteamAccount.Current(install)", StringComparison.Ordinal) &&
                  sessionRunner.Contains("start.Environment[SteamAccount.EnvironmentVariable]", StringComparison.Ordinal),
                "The native session runner probes instead of using only a cached account, or does not pass it on.");
            var playSession = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Session", "PlaySession.cs"));
            check(playSession.Contains("SteamAccount.TakeProbeResult(privateAccount)", StringComparison.Ordinal) &&
                  playSession.Contains("Environment.SetEnvironmentVariable(SteamAccount.EnvironmentVariable", StringComparison.Ordinal),
                "The session's own ownership probe does not adopt its account into the session environment.");
            var modsPage = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Launcher", "ModsPage.cs"));
            check(modsPage.Contains("SteamAccount.Current()", StringComparison.Ordinal) &&
                  !modsPage.Contains("SteamAccount.Current(new InstallLayout", StringComparison.Ordinal),
                "The Mods page starts a probe instead of using only a fresh account cache.");
            check(modsPage.Contains("OperatingSystem.IsLinux() && account == null", StringComparison.Ordinal) &&
                  modsPage.Contains("Premium items are checked against the signed-in Steam account when the session starts.", StringComparison.Ordinal),
                "The Mods page shows premium content as locked when Linux has no fresh account.");
        });

        test("Every account consumer goes through SteamAccount, and the probe's private file stays out of the report", () =>
        {
            if (RepositoryRoot() is not { } repository) { Console.WriteLine("  (skipped: no repository root above the test)"); return; }
            var consumers = new[]
            {
                Path.Combine("src", "HellForge.Core", "SteamOwnershipRecord.cs"),
                Path.Combine("src", "HellForge.Core", "GameSettings.cs"),
                Path.Combine("src", "HellForge.Session", "PlaySession.cs"),
                Path.Combine("src", "HellForge.Launcher", "InventorySyncFlow.cs"),
                Path.Combine("src", "HellForge.LocalBackend", "Program.cs"),
                Path.Combine("src", "HellForge.Cli", "Program.cs"),
                Path.Combine("src", "HellForge.Launcher", "ModsPage.cs")
            };
            foreach (var relative in consumers)
            {
                var text = File.ReadAllText(Path.Combine(repository, relative));
                check(text.Contains("SteamAccount.Current", StringComparison.Ordinal) ||
                      text.Contains("GameSettings.SteamControls", StringComparison.Ordinal), relative + " does not go through SteamAccount.");
                check(!text.Contains(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", StringComparison.Ordinal),
                    relative + " still reads the account registry directly.");
            }

            var accountImport = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Launcher", "AccountImportFlow.cs"));
            check(accountImport.Contains("SteamAccount.Current(new InstallLayout", StringComparison.Ordinal) &&
                  accountImport.Contains("start.Environment[SteamAccount.EnvironmentVariable]", StringComparison.Ordinal),
                "The launcher's direct backend commands do not receive the account.");
            var ownership = File.ReadAllText(Path.Combine(repository, "src", "HellForge.Core", "SteamOwnershipCheck.cs"));
            check(ownership.Contains("request.PrivateResult", StringComparison.Ordinal) &&
                  ownership.Contains("SteamAccount.RememberProbe(privateResult)", StringComparison.Ordinal) &&
                  ownership.Contains("privateResult = null", StringComparison.Ordinal) &&
                  ownership.Contains("useWinePaths: false, prefix: \"\", privateResult", StringComparison.Ordinal),
                "The ownership probes do not read and delete both private account results.");
            var probe = File.ReadAllText(Path.Combine(repository, "src", "HellForge.SteamProbe", "Program.cs"));
            check(probe.Contains("privateAccountPath", StringComparison.Ordinal) &&
                  probe.Contains("accountHash = userId == 0 ? null", StringComparison.Ordinal),
                "The probe does not separate its private account id from the hashed report.");
        });
    }
}
