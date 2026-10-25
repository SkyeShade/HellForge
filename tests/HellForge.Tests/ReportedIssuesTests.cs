using System.Text.Json.Nodes;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core;

// Reported problems (SkyeShade/HellForge issues), each against the behaviour that fixed it.
static class ReportedIssuesTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The Peacemaker's ammunition rows name the Knight and Redeemer only for when they equip it (bug-report-0035)", () =>
        {
            var objects = EditorCatalog.Build()["objects"]!.AsArray().Select(o => o!).ToList();
            IEnumerable<JsonNode> RowsOf(JsonNode node) => node switch
            {
                JsonObject o when o["key"] != null && o["cat"] != null => [o],
                JsonObject o => o.SelectMany(p => p.Value is null ? [] : RowsOf(p.Value)),
                JsonArray a => a.SelectMany(n => n is null ? [] : RowsOf(n)),
                _ => [],
            };
            JsonNode Row(string weapon, string key) => RowsOf(objects.Single(o => o["name"]!.GetValue<string>() == weapon)["page"]!)
                .First(r => r["key"]!.GetValue<string>() == key);
            var damage = Row("P-2 Peacemaker", "damage.standard_damage");
            var also = damage["also"]?.AsArray().Select(a => a!.GetValue<string>()).ToList() ?? [];
            var alsoIf = damage["alsoIf"]?.AsArray().Select(a => a!.GetValue<string>()).ToList() ?? [];
            check(!also.Any(a => a.Contains("Knight") || a.Contains("Redeemer")) && alsoIf.Any(a => a.Contains("Knight")) && alsoIf.Any(a => a.Contains("Redeemer")) &&
                damage["alsoAmmo"]!.GetValue<string>() == "PISTOL-SMG 9x20mm. HOLLOW POINT",
                $"Peacemaker standard damage: also [{string.Join(", ", also)}], when equipped [{string.Join(", ", alsoIf)}]");
            check(damage["shared"]!.GetValue<bool>(), "The row is still marked shared.");
            check(Row("AR-23 Liberator", "damage.standard_damage")["alsoIf"] == null, "An ammunition no other weapon lists stays unconditional.");
        });
        test("A drive that fills up while patching asks for space instead of Steam's download (hpatchz exit code 24)", () =>
        {
            var output = Path.Combine(root, "space", "bundles.00.nxa.partial");
            var full = Downpatcher.PatchFailed("data/bundles.00.nxa", 24, output);
            check(full is NotEnoughSpaceException && full.Message.Contains("ran out of space", StringComparison.Ordinal) &&
                full.Message.Contains(Path.GetPathRoot(Path.GetFullPath(output))!, StringComparison.OrdinalIgnoreCase), "Exit code 24: " + full.Message);
            var bad = Downpatcher.PatchFailed("data/bundles.00.nxa", 11, output);
            check(bad is InvalidDataException && bad.Message.Contains("validate the game in Steam", StringComparison.Ordinal), "Another exit code: " + bad.Message);
            // the check before writing: a drive's free space, with folders on one drive added up
            DiskSpace.Require((root, 1), (root, 1));
            try { DiskSpace.Require((root, long.MaxValue / 4), (root, 1)); check(false, "An impossible amount of space was granted."); }
            catch (NotEnoughSpaceException error) { check(error.Message.Contains("more is needed", StringComparison.Ordinal), error.Message); }
        });

        test("A spreadsheet open in Excel beside a mod leaves the modlist code and the source hash readable (issue 18)", () =>
        {
            var mods = Path.Combine(root, "issue18", "mods");
            var folder = Directory.CreateDirectory(Path.Combine(mods, "a.sheet")).FullName;
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), """{"format":1,"id":"a.sheet","name":"Sheet","version":"1","gameplay":true}""");
            Directory.CreateDirectory(Path.Combine(folder, "data"));
            File.WriteAllText(Path.Combine(folder, "data", "9ba626afa44a3aa3.patch_0"), "patch");
            Directory.CreateDirectory(Path.Combine(folder, "src"));
            File.WriteAllText(Path.Combine(folder, "src", "mod.c"), "int x;");
            File.WriteAllText(Path.Combine(folder, "dps.xlsx"), "sheet");
            File.WriteAllText(Path.Combine(folder, "src", "notes.xlsx"), "sheet");
            string Digest() => ModCatalog.Digest(ModCatalog.Load(mods).Mods);
            var before = Digest();
            var (loaded, problems) = ModCatalog.Load(mods);
            check(before != "none" && loaded.Count == 1 && problems.Count == 0, "The mod did not load: " + string.Join(" | ", problems));
            var sources = ModBuilds.SourceHash(folder);
            // as Excel holds them: the document read/write, sharing reads; its owner file (~$name, hidden) with no sharing
            using (new FileStream(Path.Combine(folder, "dps.xlsx"), FileMode.Open, FileAccess.ReadWrite, FileShare.Read))
            using (new FileStream(Path.Combine(folder, "src", "notes.xlsx"), FileMode.Open, FileAccess.ReadWrite, FileShare.Read))
            using (new FileStream(Path.Combine(folder, "~$dps.xlsx"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            using (new FileStream(Path.Combine(folder, "src", "~$notes.xlsx"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            using (new FileStream(Path.Combine(folder, "A1B2C3.tmp"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            {
                check(Digest() == before, "The modlist code changed (or failed) while the files were open.");
                check(ModBuilds.SourceHash(folder) == sources, "The source hash changed (or failed) while the files were open.");
            }
            File.Delete(Path.Combine(folder, "~$dps.xlsx"));
            File.Delete(Path.Combine(folder, "src", "~$notes.xlsx"));
            File.Delete(Path.Combine(folder, "A1B2C3.tmp"));
            File.WriteAllText(Path.Combine(folder, "dps.xlsx"), "changed sheet");
            check(Digest() != before, "The spreadsheet itself is still part of the mod.");
        });

        test("In the Linux network namespace, missing mod tools are not fetched: the problem says how to get them (issue 20)", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var sdk = repository with { Tools = Directory.CreateDirectory(Path.Combine(root, "issue20", "tools")).FullName };
            var install = new InstallLayout(Directory.CreateDirectory(Path.Combine(root, "issue20", "install")).FullName);
            var folder = ModKit.Create(sdk, Directory.CreateDirectory(Path.Combine(install.Root, "mods")).FullName, "tester.linux-source", "Linux Source");
            Environment.SetEnvironmentVariable(LinuxNetworkBlock.EnvVar, "1");
            try
            {
                var (ready, problems) = ModBuilds.Prepare(install, sdk, [ModCatalog.Parse(folder, EntityProfile.Current)], "25480438");
                check(ready.Count == 0 && problems is [var problem] && problem.Contains("hellforge-linux.sh") && !problem.Contains("ziglang.org"),
                    "Not the namespace's refusal, but how to get the tools: " + string.Join(" | ", problems));
                check(Directory.GetFileSystemEntries(sdk.Tools).Length == 0, "A download was started.");
            }
            finally { Environment.SetEnvironmentVariable(LinuxNetworkBlock.EnvVar, null); }
        });

        test("A swapped upscaler (DLSS, XeSS, FSR) still verifies; any other changed file does not (issue 13)", () =>
        {
            var game = Path.Combine(root, "issue13", "game");
            Directory.CreateDirectory(Path.Combine(game, "bin"));
            static string Sha1(byte[] bytes) => Convert.ToHexString(SHA1.HashData(bytes));
            var dlss = Encoding.UTF8.GetBytes("dlss 3.7");
            var other = Encoding.UTF8.GetBytes("game library");
            var build = new SupportedBuild(1, "553850", "25480438", "", "", [],
                [new SupportedFile("bin/nvngx_dlss.dll", dlss.Length, Sha1(dlss), 1), new SupportedFile("bin/other.dll", other.Length, Sha1(other), 1)]);
            File.WriteAllBytes(Path.Combine(game, "bin", "nvngx_dlss.dll"), dlss);
            File.WriteAllBytes(Path.Combine(game, "bin", "other.dll"), other);
            check(build.Verify(game).Count == 0 && build.IsComplete(game), "The build's own files did not verify.");
            File.WriteAllText(Path.Combine(game, "bin", "nvngx_dlss.dll"), "dlss 4.0, a newer and longer library");
            check(build.Verify(game).Count == 0 && build.IsComplete(game), "A newer DLSS was refused.");
            check(build.Files.All(f => SupportedBuild.Accepts(Path.Combine(game, f.Path), f)), "A repair would put the old DLSS back.");
            File.WriteAllText(Path.Combine(game, "bin", "other.dll"), "game librarY");
            check(build.Verify(game) is [var changed] && changed == "changed: bin/other.dll", "A changed game file verified.");
            check(!SupportedBuild.Accepts(Path.Combine(game, "bin", "other.dll"), build.Files[1]), "A repair would keep a changed game file.");
            File.Delete(Path.Combine(game, "bin", "nvngx_dlss.dll"));
            check(build.Verify(game).Contains("missing: bin/nvngx_dlss.dll") && !build.IsComplete(game), "A missing upscaler was not noticed.");
            check(new[] { "bin/nvngx_dlss.dll", "bin/libxess.dll", "bin/amd_fidelityfx_upscaler_dx12.dll" }.All(p =>
                SupportedBuild.IsReplaceable(p) && SupportedBuild.Current.Files.Any(f => f.Path == p)), "The upscalers are not the build's.");
        });
    }
}
