using System.Text.Json.Nodes;
using HellForge.Core;

// Reusing a recent Steam ownership check (issue #43, SteamOwnershipRecord): a session skips asking Steam only for the same
// installation, Steam account and game build within the limit; everything else runs the full check, and a failed check is
// never recorded.
static class OwnershipRecordTests
{
    const uint Account = 123456789, OtherAccount = 987654321;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        InstallLayout Install(string name)
        {
            var install = new InstallLayout(Path.Combine(root, name));
            install.Create(SupportedBuild.Current.BuildId);
            return install;
        }
        DateTime Created(InstallLayout install) => install.ReadMarker().CreatedUtc;
        var now = new DateTime(2026, 10, 25, 12, 0, 0, DateTimeKind.Utc);

        test("A recent ownership check stands in for the full check only for the same installation, account and build within 7 days", () =>
        {
            var install = Install("ownership-reuse");
            var build = SupportedBuild.Current.BuildId;
            var record = new OwnershipConfirmation(install.Root, Created(install), SteamOwnershipCheck.AccountHash(Account), build, now.AddDays(-6.9));
            var context = new OwnershipContext(install.Root, Created(install), Account, build, now);
            check(SteamOwnershipRecord.Problem(record, context) == null, "A 6.9-day-old confirmation of this installation, account and build was refused.");
            check(SteamOwnershipRecord.Problem(record, context with { InstallRoot = install.Root.ToUpperInvariant() }) == null,
                "The installation's path in another letter case is the same installation (Windows).");
            check(SteamOwnershipRecord.Problem(record with { ConfirmedUtc = now.AddMinutes(3) }, context) == null, "A clock a few minutes behind was refused.");
            check(SteamOwnershipRecord.Limit == TimeSpan.FromDays(7), "The limit is 7 days.");
            foreach (var (why, value, at) in new (string, OwnershipConfirmation?, OwnershipContext)[]
            {
                ("no record", null, context),
                ("another installation folder", record with { InstallRoot = Path.Combine(root, "elsewhere") }, context),
                ("the installation made again in the same folder", record with { InstallCreatedUtc = record.InstallCreatedUtc.AddSeconds(1) }, context),
                ("an installation whose marker does not read", record, context with { InstallCreatedUtc = DateTime.MinValue }),
                ("Steam not running or signed out", record, context with { ActiveAccount = null }),
                ("another Steam account", record, context with { ActiveAccount = OtherAccount }),
                ("another game build", record with { BuildId = "1" }, context),
                ("exactly the limit", record with { ConfirmedUtc = now - SteamOwnershipRecord.Limit }, context),
                ("older than the limit", record with { ConfirmedUtc = now.AddDays(-30) }, context),
                ("dated in the future (a clock set back)", record with { ConfirmedUtc = now.AddHours(1) }, context)
            })
                check(SteamOwnershipRecord.Problem(value, at) != null, "Reused: " + why);
        });

        test("The ownership record round-trips sealed for this Windows user; an edited, unsealed, broken or missing record counts as none", () =>
        {
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: DPAPI is Windows only)"); return; }
            var install = Install("ownership-seal");
            var path = install.OwnershipRecord;
            check(path.StartsWith(install.Temp + Path.DirectorySeparatorChar, StringComparison.Ordinal) && !path.StartsWith(install.Game, StringComparison.Ordinal),
                "The record is the installation's own state, not in the game folder.");
            check(SteamOwnershipRecord.Read(path) == null, "A missing record read as one.");
            var record = new OwnershipConfirmation(install.Root, Created(install), SteamOwnershipCheck.AccountHash(Account), SupportedBuild.Current.BuildId, now);
            check(SteamOwnershipRecord.Write(path, record), "The record was not written.");
            check(SteamOwnershipRecord.Read(path) == record, "The record did not read back as written.");
            check(!File.ReadAllText(path).Contains(install.Root.Replace("\\", "\\\\"), StringComparison.OrdinalIgnoreCase) &&
                  !File.ReadAllText(path).Contains(record.AccountHash, StringComparison.Ordinal), "The record's content is not sealed.");

            var sealedText = (string)JsonNode.Parse(File.ReadAllText(path))!["sealed"]!;
            var bytes = Convert.FromBase64String(sealedText);
            bytes[bytes.Length / 2] ^= 0x01;
            File.WriteAllText(path, new JsonObject { ["schemaVersion"] = 1, ["sealed"] = Convert.ToBase64String(bytes) }.ToJsonString());
            check(SteamOwnershipRecord.Read(path) == null, "A record with one changed byte was read.");
            // what someone would write by hand: the fields in plain text, unsealed
            var plain = new JsonObject
            {
                ["schemaVersion"] = 1, ["installRoot"] = record.InstallRoot, ["installCreatedUtc"] = record.InstallCreatedUtc.ToString("o"),
                ["accountHash"] = record.AccountHash, ["buildId"] = record.BuildId, ["confirmedUtc"] = now.ToString("o")
            };
            File.WriteAllText(path, plain.ToJsonString());
            check(SteamOwnershipRecord.Read(path) == null, "An unsealed record was read.");
            File.WriteAllText(path, new JsonObject { ["schemaVersion"] = 1, ["sealed"] = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(plain.ToJsonString())) }.ToJsonString());
            check(SteamOwnershipRecord.Read(path) == null, "A record encoded but not sealed was read.");
            foreach (var broken in new[] { "{not json", "[]", "{\"sealed\": \"not base64!\"}", "{\"sealed\": 5}" })
            {
                File.WriteAllText(path, broken);
                check(SteamOwnershipRecord.Read(path) == null, "A broken record was read: " + broken);
            }
            check(SteamOwnershipRecord.Forget(path) && !File.Exists(path) && SteamOwnershipRecord.Forget(path), "Forget did not remove the record.");
        });

        test("Only a passed check is recorded, for the account Steam confirmed; a failed check removes the record", () =>
        {
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: DPAPI is Windows only)"); return; }
            var install = Install("ownership-after-check");
            Directory.CreateDirectory(install.Logs);
            var evidence = Path.Combine(install.Logs, "ownership.json");
            var started = DateTime.UtcNow;
            string Report(string status, bool confirmed, string? account) =>
                new JsonObject
                {
                    ["status"] = status, ["checkedUtc"] = started.AddSeconds(1).ToString("o"), ["appId"] = SteamOwnershipCheck.AppId,
                    ["localClientOwnershipConfirmed"] = confirmed, ["accountHash"] = account
                }.ToJsonString();

            File.WriteAllText(evidence, Report(SteamOwnershipCheck.Confirmed, true, SteamOwnershipCheck.AccountHash(Account)));
            check(SteamOwnershipRecord.AfterCheck(install, evidence, passed: true, started), "A passed check was not recorded.");
            var record = SteamOwnershipRecord.Read(install.OwnershipRecord);
            check(record != null && record.AccountHash == SteamOwnershipCheck.AccountHash(Account) && record.BuildId == SupportedBuild.Current.BuildId &&
                  record.InstallRoot == install.Root && record.InstallCreatedUtc == Created(install) && record.ConfirmedUtc == started,
                "The record does not name this check: " + record);
            var context = new OwnershipContext(install.Root, Created(install), Account, SupportedBuild.Current.BuildId, started.AddDays(1));
            check(SteamOwnershipRecord.Problem(record, context) == null, "The next day's session would not reuse it.");
            check(SteamOwnershipRecord.Problem(record, context with { ActiveAccount = OtherAccount }) != null, "Another account would reuse it.");

            // the check failed (the script's exit code), whatever its evidence says: no record, and the earlier one is gone
            check(!SteamOwnershipRecord.AfterCheck(install, evidence, passed: false, started) && !File.Exists(install.OwnershipRecord),
                "A failed check left a record.");
            foreach (var (why, json) in new[]
            {
                ("Steam did not confirm", Report("ownership_unverified", false, SteamOwnershipCheck.AccountHash(Account))),
                ("a probe that names no account", Report(SteamOwnershipCheck.Confirmed, true, null)),
                ("evidence from an earlier check", Report(SteamOwnershipCheck.Confirmed, true, SteamOwnershipCheck.AccountHash(Account))
                    .Replace(started.AddSeconds(1).ToString("o"), started.AddMinutes(-10).ToString("o"), StringComparison.Ordinal))
            })
            {
                SteamOwnershipRecord.AfterCheck(install, WriteEvidence(SteamOwnershipCheck.Confirmed), passed: true, started);
                File.WriteAllText(evidence, json);
                check(!SteamOwnershipRecord.AfterCheck(install, evidence, passed: true, started) && !File.Exists(install.OwnershipRecord),
                    "Recorded: " + why);
            }
            File.Delete(evidence);
            check(!SteamOwnershipRecord.AfterCheck(install, evidence, passed: true, started), "A check that wrote no evidence was recorded.");

            string WriteEvidence(string status)
            {
                File.WriteAllText(evidence, Report(status, true, SteamOwnershipCheck.AccountHash(Account)));
                return evidence;
            }
        });

        test("The account a check names is Steam's account id hashed: the SteamID64's low 32 bits, as Steam's ActiveUser holds it", () =>
        {
            const ulong steamId64 = 76561197960265728UL + Account;
            check(SteamOwnershipCheck.AccountHash(unchecked((uint)steamId64)) == SteamOwnershipCheck.AccountHash(Account), "The SteamID64 and the account id differ.");
            check(SteamOwnershipCheck.AccountHash(Account) != SteamOwnershipCheck.AccountHash(OtherAccount), "Two accounts hash alike.");
            check(SteamOwnershipCheck.AccountHash(Account).Length == 32 && !SteamOwnershipCheck.AccountHash(Account).Contains(Account.ToString(), StringComparison.Ordinal),
                "The hash carries the account id.");
        });

        test("A session can be asked for the full ownership check", () =>
        {
            var install = Install("ownership-full-option");
            var paths = new InstallSessionPaths(install);
            Directory.CreateDirectory(install.App);
            File.WriteAllText(Path.Combine(install.App, "profile-templates.json"), """{"templates": {"starter": {"description": "New player", "items": []}}}""");
            var profile = ProfileStore.List(paths)[0];
            var full = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, [], FullOwnershipCheck: true));
            var usual = SessionRunner.Arguments(paths, profile, new(false, false, ProfileStore.WarFrozen, false, false, []));
            check(full[^1] == "--full-ownership-check" && !usual.Contains("--full-ownership-check"), "Full check only when asked: " + string.Join(" ", full));
        });
    }
}
