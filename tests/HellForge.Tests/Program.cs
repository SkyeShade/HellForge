using HellForge.Core;

// Dependency-free integration checks. Every game file here is synthetic.
var root = Path.GetFullPath(Path.Combine("artifacts", "tests", Guid.NewGuid().ToString("N")));
Directory.CreateDirectory(root);
var passed = 0;
var failed = 0;
void Check(bool condition, string reason)
{
    if (!condition) throw new Exception(reason);
}
void Reject(Action action)
{
    try { action(); }
    catch (Exception error) when (error is InvalidDataException or IOException) { return; }
    throw new Exception("Expected rejection.");
}
void Test(string name, Action action)
{
    try { action(); passed++; Console.WriteLine($"PASS {name}"); }
    catch (Exception error) { failed++; Console.WriteLine($"FAIL {name}: {error.Message}"); }
}
(string Manifest, string Source, string Home) Fixture(string name)
{
    var home = Path.Combine(root, name);
    var steamApps = Path.Combine(home, "Steam", "steamapps");
    var source = Path.Combine(steamApps, "common", "Helldivers 2");
    Directory.CreateDirectory(Path.Combine(source, "bin"));
    Directory.CreateDirectory(Path.Combine(source, "data", "game"));
    File.WriteAllText(Path.Combine(source, LocalInstall.Exe), "synthetic executable");
    File.WriteAllText(Path.Combine(source, LocalInstall.GameDll), "synthetic game library");
    File.WriteAllText(Path.Combine(source, "data", "asset"), "synthetic asset");
    var manifest = Path.Combine(steamApps, "appmanifest_553850.acf");
    File.WriteAllText(manifest, """
        "AppState" { "appid" "553850" "StateFlags" "4" "installdir" "Helldivers 2" "buildid" "12345" }
        """);
    return (manifest, source, home);
}

Test("Steam ownership requires the correct app, live subscription, the owner or Steam Family Sharing, and no temporary mode", () =>
{
    var valid = new SteamOwnershipObservation(553850, true, true, true, false, false, false);
    Check(SteamOwnershipPolicy.ConfirmsCurrentClientOwner(valid), "Live matching owner rejected.");
    var borrowed = valid with { SameOwner = false, FamilyShared = true };
    Check(SteamOwnershipPolicy.ConfirmsCurrentClientOwner(borrowed), "A family member's shared licence rejected.");
    foreach (var rejected in new[] { valid with { AppId = 480 }, valid with { LoggedOn = false },
        valid with { Subscribed = false }, valid with { SameOwner = false }, borrowed with { Subscribed = false },
        valid with { FreeWeekend = true }, valid with { Cafe = true } })
        Check(!SteamOwnershipPolicy.ConfirmsCurrentClientOwner(rejected), "Incomplete ownership observation accepted.");
});

Test("The ownership check passes only on fresh evidence that confirms this app; a check that wrote nothing fails", () =>
{
    var folder = Directory.CreateDirectory(Path.Combine(root, "ownership-evidence")).FullName;
    var started = DateTime.UtcNow;
    string Evidence(string name, string json) { var path = Path.Combine(folder, name); File.WriteAllText(path, json); return path; }
    string Report(string status, bool confirmed, uint app, DateTime at) =>
        $$"""{"status": "{{status}}", "checkedUtc": "{{at:o}}", "appId": {{app}}, "localClientOwnershipConfirmed": {{(confirmed ? "true" : "false")}}}""";
    Check(SteamOwnershipCheck.Problem(Evidence("ok.json", Report(SteamOwnershipCheck.Confirmed, true, 553850, started.AddSeconds(1))), started) == null,
        "Good evidence refused.");
    // Wine's powershell.exe returns 0 having run nothing: no evidence at all
    Check(SteamOwnershipCheck.Problem(Path.Combine(folder, "none.json"), started) != null, "Missing evidence passed.");
    foreach (var (name, json) in new[]
    {
        ("unverified.json", Report("ownership_unverified", false, 553850, started)),
        ("half.json", Report(SteamOwnershipCheck.Confirmed, false, 553850, started)),
        ("other-app.json", Report(SteamOwnershipCheck.Confirmed, true, 480, started)),
        ("stale.json", Report(SteamOwnershipCheck.Confirmed, true, 553850, started.AddMinutes(-10))),
        ("broken.json", "{not json"),
        ("array.json", "[]")
    })
        Check(SteamOwnershipCheck.Problem(Evidence(name, json), started) != null, "Evidence passed: " + name);
    // the probe path (under Wine) takes only the supported build's own steam_api64.dll
    var fake = Evidence("steam_api64.dll", "not Valve's");
    Reject(() => SteamOwnershipCheck.RunProbe(Path.Combine(folder, "probe.exe"), fake, folder, Path.Combine(folder, "e.json"), TimeSpan.FromSeconds(1)));
});

Test("Setup finds the downpatchable game: Steam's flags as bits, every library, the patch by build id or by file hashes", () =>
{
    // a queued or paused update leaves the files usable; updating, validating, missing files or a running game do not
    foreach (var usable in new uint[] { 4, 6, 516 }) Check(LocalInstall.Unusable(usable) == null, $"Flags {usable} refused.");
    foreach (var busy in new uint[] { 0, 1026, 1028, 36, 132, 68, 1048580, 131076 }) Check(LocalInstall.Unusable(busy) != null, $"Flags {busy} accepted.");
    // a stale copy in Steam's own library (another build) and the patch's source build in a second library
    var (staleManifest, _, home) = Fixture("setup-libraries-a");
    var (patchable, _, _) = Fixture("setup-libraries-b");
    File.WriteAllText(patchable, File.ReadAllText(patchable).Replace("\"buildid\" \"12345\"", $"\"buildid\" \"{Downpatcher.Packs[0].SourceBuild}\""));
    var steamRoot = Path.Combine(home, "Steam");
    var second = Path.GetDirectoryName(Path.GetDirectoryName(patchable)!)!;
    File.WriteAllText(Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf"),
        "\"libraryfolders\" { \"0\" { \"path\" \"" + steamRoot.Replace("\\", "\\\\") + "\" } \"1\" { \"path\" \"" + second.Replace("\\", "\\\\") + "\" } }");
    var game = SteamLocator.FindGame(steamRoot);
    Check(game != null && game.ManifestPath == LocalInstall.FullPath(patchable) && Downpatcher.FindPack(game.Report.Pin) != null,
        "The patchable copy was not chosen: " + game?.ManifestPath);
    // with only the stale copy: it, and no patch for it
    File.Delete(patchable);
    game = SteamLocator.FindGame(steamRoot);
    Check(game != null && game.ManifestPath == LocalInstall.FullPath(staleManifest) && Downpatcher.FindPack(game.Report.Pin) == null, "The stale copy was not the fallback.");
    // a copy whose manifest names another build but whose files are the patch's source build
    var pack = Downpatcher.Packs[0];
    Check(Downpatcher.FindPack(new BuildPin("1", pack.SourceExeSha256.ToLowerInvariant(), pack.SourceGameDllSha256)) == pack, "The patch was not found by its hashes.");
    Check(Downpatcher.FindPack(new BuildPin("1", pack.SourceExeSha256, "00")) == null, "A half match found the patch.");
    // a busy game (Steam updating it) is reported, not skipped as absent
    File.WriteAllText(staleManifest, File.ReadAllText(staleManifest).Replace("\"StateFlags\" \"4\"", "\"StateFlags\" \"1028\""));
    try { SteamLocator.FindGame(steamRoot); Check(false, "A game Steam is updating was used."); }
    catch (InvalidDataException error) { Check(error.Message.Contains("updating"), error.Message); }
});

Test("Valve metadata supports comments, escapes, and quoted braces", () =>
{
    var data = ValveData.Parse("""
        // Steam fixture
        "root" { "path" "C:\\Games\\Steam" "brace" "}" "quote" "a\"b" }
        """).Children["root"];
    Check(data.Required("path") == @"C:\Games\Steam", "Escaped path was damaged.");
    Check(data.Required("brace") == "}" && data.Required("quote") == "a\"b", "Quoted values were damaged.");
});
Test("Malformed and ambiguous Steam metadata fails", () =>
{
    foreach (var text in new[] { "\"x\" {", "\"x\"", "\"x\" \"1\" \"X\" \"2\"", "}", "\"x" })
        Reject(() => ValveData.Parse(text));
});
Test("Discovery supports secondary Steam libraries", () =>
{
    var fixture = Fixture("secondary");
    var primary = Path.Combine(fixture.Home, "Primary");
    Directory.CreateDirectory(Path.Combine(primary, "steamapps"));
    var library = Path.Combine(fixture.Home, "Steam").Replace("\\", "\\\\");
    File.WriteAllText(Path.Combine(primary, "steamapps", "libraryfolders.vdf"),
        $"\"libraryfolders\" {{ \"1\" {{ \"path\" \"{library}\" }} }}");
    Check(LocalInstall.FindManifests(primary).SequenceEqual([fixture.Manifest]), "Secondary library not discovered.");
});
Test("Inspection fingerprints files without claiming ownership or isolation", () =>
{
    var fixture = Fixture("inspect");
    var before = LocalInstall.Hash(Path.Combine(fixture.Source, LocalInstall.Exe));
    var report = LocalInstall.Inspect(fixture.Manifest);
    Check(report.Pin.BuildId == "12345" && report.Pin.ExeSha256 == before, "Incorrect build identity.");
    Check(report.FileCount == 3 && report.TotalBytes > 0, "Incorrect inventory.");
    Check(!report.ReadyToLaunch && report.OwnershipStatus == "not_verified_local_metadata_only" &&
        report.IsolationStatus == "not_implemented", "Unsupported readiness claim.");
    Check(LocalInstall.Hash(Path.Combine(fixture.Source, LocalInstall.Exe)) == before, "Source changed.");
});
Test("Wrong app, incomplete install, and traversal are rejected", () =>
{
    var fixture = Fixture("metadata");
    var original = File.ReadAllText(fixture.Manifest);
    foreach (var changed in new[] { original.Replace("553850", "999"), original.Replace("\"4\"", "\"2\""),
        original.Replace("Helldivers 2", "../escape") })
    {
        File.WriteAllText(fixture.Manifest, changed);
        Reject(() => LocalInstall.Inspect(fixture.Manifest));
    }
});
Test("Missing required game binary is rejected", () =>
{
    var fixture = Fixture("missing");
    File.Delete(Path.Combine(fixture.Source, LocalInstall.GameDll));
    Reject(() => LocalInstall.Inspect(fixture.Manifest));
});
Test("Preparation verifies copy bytes and leaves source unchanged", () =>
{
    var fixture = Fixture("copy");
    var report = LocalInstall.Inspect(fixture.Manifest);
    var destination = Path.Combine(fixture.Home, "HellForge");
    var receipt = CopyPreparer.Prepare(fixture.Manifest, destination, report.Pin);
    Check(receipt.Files.Count == 3 && !receipt.ReadyToLaunch, "Incorrect copy receipt.");
    foreach (var file in receipt.Files)
    {
        Check(LocalInstall.Hash(Path.Combine(destination, "game", file.RelativePath)) == file.Sha256, "Wrong copied bytes.");
        Check(LocalInstall.Hash(Path.Combine(fixture.Source, file.RelativePath)) == file.Sha256, "Source was altered.");
    }
    Check(File.Exists(Path.Combine(destination, "copy-receipt.json")), "Missing receipt.");
    Check(!File.Exists(Path.Combine(destination, "preparing.lock")), "Successful copy left incomplete marker.");
});
Test("Build and executable mismatches fail before creating destination", () =>
{
    var fixture = Fixture("pins");
    var report = LocalInstall.Inspect(fixture.Manifest);
    var destination = Path.Combine(fixture.Home, "HellForge");
    Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, report.Pin with { BuildId = "54321" }));
    Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, report.Pin with { ExeSha256 = new string('0', 64) }));
    Check(!Directory.Exists(destination), "Rejected pin created destination.");
});
Test("Overlapping, existing, and Steam-managed destinations fail", () =>
{
    var fixture = Fixture("paths");
    var pin = LocalInstall.Inspect(fixture.Manifest).Pin;
    foreach (var destination in new[] { fixture.Source, Path.Combine(fixture.Source, "HellForge"), fixture.Home,
        Path.Combine(Path.GetDirectoryName(fixture.Manifest)!, "common", "HellForge") })
        Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, pin));
    Check(!Directory.Exists(Path.Combine(fixture.Source, "HellForge")), "Source contains rejected destination.");
});
Test("Interrupted copy retains marker and cannot be resumed as complete", () =>
{
    var fixture = Fixture("interrupted");
    var pin = LocalInstall.Inspect(fixture.Manifest).Pin;
    var destination = Path.Combine(fixture.Home, "HellForge");
    Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, pin, (_, _) => throw new IOException("Injected interruption")));
    Check(File.Exists(Path.Combine(destination, "preparing.lock")), "Missing incomplete marker.");
    Check(!File.Exists(Path.Combine(destination, "copy-receipt.json")), "Interrupted copy has receipt.");
    Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, pin));
});
Test("Source change during copy prevents completion", () =>
{
    var fixture = Fixture("changed");
    var pin = LocalInstall.Inspect(fixture.Manifest).Pin;
    var destination = Path.Combine(fixture.Home, "HellForge");
    Reject(() => CopyPreparer.Prepare(fixture.Manifest, destination, pin, (done, _) =>
    {
        if (done == 1) File.AppendAllText(Path.Combine(fixture.Source, LocalInstall.Exe), "changed");
    }));
    Check(!File.Exists(Path.Combine(destination, "copy-receipt.json")), "Changed source was accepted.");
});

// Steam content manifests: protobuf sections with a payload (files) and metadata (depot, manifest).
static byte[] Varint(ulong value)
{
    var bytes = new List<byte>();
    do { var current = (byte)(value & 0x7F); value >>= 7; bytes.Add(value != 0 ? (byte)(current | 0x80) : current); } while (value != 0);
    return bytes.ToArray();
}
static byte[] Number(int field, ulong value) => [.. Varint((ulong)(field << 3)), .. Varint(value)];
static byte[] Blob(int field, byte[] value) => [.. Varint((ulong)(field << 3 | 2)), .. Varint((ulong)value.Length), .. value];
static byte[] Section(uint magic, byte[] body) => [.. BitConverter.GetBytes(magic), .. BitConverter.GetBytes((uint)body.Length), .. body];
static byte[] DepotManifestBytes(uint depot, ulong manifest, params (string Name, byte[] Content, uint Flags)[] files)
{
    var payload = new List<byte>();
    foreach (var (name, content, flags) in files)
    {
        var chunk = Blob(1, System.Security.Cryptography.SHA1.HashData(content)).Concat(Number(3, 0)).Concat(Number(4, (ulong)content.Length)).ToArray();
        payload.AddRange(Blob(1, Blob(1, System.Text.Encoding.UTF8.GetBytes(name)).Concat(Number(2, (ulong)content.Length)).Concat(Number(3, flags))
            .Concat(Blob(5, System.Security.Cryptography.SHA1.HashData(content))).Concat(Blob(6, chunk)).ToArray()));
    }
    var metadata = Number(1, depot).Concat(Number(2, manifest)).Concat(Number(4, 0)).ToArray();
    return [.. Section(0x71F617D0, payload.ToArray()), .. Section(0x1F4812BE, metadata), .. BitConverter.GetBytes(0x32C415ABu)];
}
// A fake Steam install of build 12345 with depot 553851 (manifest 42) and its cached manifest.
(string Manifest, string Source, string DepotCache) DepotFixture(string name)
{
    var (manifest, source, home) = Fixture(name);
    File.WriteAllText(manifest, """
        "AppState" { "appid" "553850" "StateFlags" "4" "installdir" "Helldivers 2" "buildid" "12345"
            "InstalledDepots" { "553851" { "manifest" "42" "size" "1" } } }
        """);
    Directory.CreateDirectory(Path.Combine(source, "bin", "GameGuard"));
    File.WriteAllText(Path.Combine(source, "bin", "GameGuard", "npsc.des"), "a module the copy never uses");
    File.WriteAllText(Path.Combine(source, "data", "asset.patch_0"), "a mod, not part of any depot");
    var depotCache = Path.Combine(home, "Steam", "depotcache");
    Directory.CreateDirectory(depotCache);
    (string, byte[], uint) Entry(string relative) => (relative.Replace('/', '\\'), File.ReadAllBytes(Path.Combine(source, relative)), 0u);
    File.WriteAllBytes(Path.Combine(depotCache, "553851_42.manifest"), DepotManifestBytes(553851, 42,
        ("bin", [], 0x40u), Entry(LocalInstall.Exe), Entry(LocalInstall.GameDll), Entry("data/asset"), Entry("bin/GameGuard/npsc.des")));
    return (manifest, source, depotCache);
}

Test("Steam depot manifests yield files, hashes and chunks, and mark directories", () =>
{
    var content = "synthetic"u8.ToArray();
    var parsed = SteamDepotManifest.Parse(DepotManifestBytes(553853, 7, ("data", [], 0x40u), ("data\\bundle", content, 0u)));
    Check(parsed.DepotId == 553853 && parsed.ManifestId == 7 && !parsed.FilenamesEncrypted, "Metadata not read.");
    Check(parsed.Files.Count == 2 && parsed.Files[0].IsDirectory && !parsed.Files[1].IsDirectory, "Directory flag not read.");
    var file = parsed.Files[1];
    Check(file.Path == "data/bundle" && file.Size == (ulong)content.Length && file.Sha1 == Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(content)),
        "File entry not read.");
    Check(file.Chunks.Count == 1 && file.Chunks[0].OriginalSize == content.Length, "Chunk not read.");
    Reject(() => SteamDepotManifest.Parse("not a manifest"u8));
});

Test("A supported build lists depot files only, pins the build, and verifies a copy", () =>
{
    var (manifest, source, depotCache) = DepotFixture("supported-build");
    var build = SupportedBuild.FromSteam(manifest, depotCache);
    Check(build.BuildId == "12345" && build.Depots.Single().ManifestId == "42", "Build or depot not recorded.");
    Check(build.Files.Count == 3 && build.Files.All(f => f.Path != "data/asset.patch_0"), "Non-depot files listed.");
    Check(build.Files.All(f => !SupportedBuild.IsUnused(f.Path)), "A depot file the copy never uses is listed.");
    Check(build.ExeSha256 == LocalInstall.Hash(Path.Combine(source, LocalInstall.Exe)), "Executable not pinned.");
    var saved = Path.Combine(root, "supported-build.json");
    build.Save(saved);
    using (var stream = File.OpenRead(saved))
    {
        var loaded = SupportedBuild.Load(stream);
        Check(loaded.BuildId == build.BuildId && loaded.ExeSha256 == build.ExeSha256 && loaded.Depots.SequenceEqual(build.Depots) &&
            loaded.Files.SequenceEqual(build.Files), "The saved list does not load back unchanged.");
    }
    Check(build.Verify(source).Count == 0, "A matching folder failed verification.");
    Check(build.IsComplete(source), "A matching folder is not complete.");
    var asset = Path.Combine(source, "data", "asset");
    var assetBytes = File.ReadAllBytes(asset);
    File.WriteAllBytes(asset + GameSettings.OriginalSuffix, assetBytes);
    File.AppendAllText(asset, " rewritten by a session");
    Check(build.IsComplete(source), "A file rewritten from its kept original made the copy incomplete.");
    File.Delete(asset + GameSettings.OriginalSuffix);
    Check(!build.IsComplete(source), "A resized file passed the completeness check.");
    File.WriteAllBytes(asset, assetBytes);
    Directory.Delete(Path.Combine(source, "bin", "GameGuard"), recursive: true);
    Check(build.Verify(source).Count == 0, "A copy without the unused depot files failed verification.");
    File.WriteAllText(Path.Combine(source, "data", "asset"), "synthetic assft");
    Check(build.Verify(source).SequenceEqual(["changed: data/asset"]), "A changed file passed verification.");
    File.WriteAllText(Path.Combine(source, LocalInstall.Exe), "other build");
    Reject(() => SupportedBuild.FromSteam(manifest, depotCache));
});

Test("The game copy takes depot files only, resumes, repairs, and rejects other builds", () =>
{
    var (manifest, source, depotCache) = DepotFixture("game-copy");
    var build = SupportedBuild.FromSteam(manifest, depotCache);
    var destination = Path.Combine(root, "game-copy-install", "game");
    long lastDone = 0, lastTotal = 0;
    GameCopier.Copy(source, destination, build, (done, total, _) => (lastDone, lastTotal) = (done, total));
    Check(build.Verify(destination).Count == 0 && lastDone == lastTotal && lastTotal == build.TotalBytes, "Copy incomplete.");
    Check(!File.Exists(Path.Combine(destination, "data", "asset.patch_0")), "A mod was copied.");
    Check(!Directory.Exists(Path.Combine(destination, "bin", "GameGuard")), "An unused depot file was copied.");
    Check(!Directory.EnumerateFiles(destination, "*.partial", SearchOption.AllDirectories).Any(), "Partial files remain.");
    File.Delete(Path.Combine(destination, "data", "asset"));
    File.WriteAllText(Path.Combine(destination, LocalInstall.GameDll), "damaged");
    GameCopier.Copy(source, destination, build);
    Check(build.Verify(destination).Count == 0, "Copy did not repair missing and damaged files.");
    File.WriteAllText(Path.Combine(source, "data", "asset"), "a different build");
    File.Delete(Path.Combine(destination, "data", "asset"));
    Reject(() => GameCopier.Copy(source, destination, build));
    Check(!File.Exists(Path.Combine(destination, "data", "asset")), "A file from another build was installed.");
    Reject(() => GameCopier.Copy(source, Path.Combine(source, "nested"), build));
});

Test("The game is copied from Steam's depot downloads of the supported build, which can be deleted after", () =>
{
    var (manifest, source, depotCache) = DepotFixture("depot-download");
    var build = SupportedBuild.FromSteam(manifest, depotCache);
    var steam = Path.Combine(root, "depot-download-steam");
    Check(SteamDepotDownload.Commands(build).SequenceEqual(build.Depots.Select(d => $"download_depot {build.AppId} {d.Id} {d.ManifestId}")),
        "The console commands do not name the pinned manifests.");
    Check(!SteamDepotDownload.IsComplete(steam, build) && SteamDepotDownload.Status(steam, build).All(d => d.Done == 0), "Nothing downloaded counts as complete.");
    // Steam writes each depot's files under steamapps/content/app_<app>/depot_<id>.
    foreach (var file in build.Files)
    {
        var target = SteamDepotDownload.Locate(steam, build, file);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(Path.Combine(source, file.Path), target);
    }
    Check(SteamDepotDownload.IsComplete(steam, build), "A complete download is not complete.");
    var destination = Path.Combine(root, "depot-download-install", "game");
    GameCopier.CopyFromDepots(steam, destination, build);
    Check(build.Verify(destination).Count == 0, "The copy from the download is not the build.");
    Reject(() => GameCopier.CopyFromDepots(steam, Path.Combine(steam, "steamapps", "common", "game"), build));
    var asset = build.Files.Single(f => f.Path == "data/asset");
    File.WriteAllText(SteamDepotDownload.Locate(steam, build, asset), "another manifest");
    File.Delete(Path.Combine(destination, "data", "asset"));
    Reject(() => GameCopier.CopyFromDepots(steam, destination, build));
    SteamDepotDownload.Delete(steam, build);
    Check(!Directory.Exists(SteamDepotDownload.ContentFolder(steam, build)), "The download was not deleted.");
});

Test("On Linux, the depot downloads Steam put under its client's folder (ubuntu12_32, steamrt64) are found and deleted too", () =>
{
    var (manifest, source, depotCache) = DepotFixture("depot-download-linux");
    var build = SupportedBuild.FromSteam(manifest, depotCache);
    var steam = Path.Combine(root, "depot-download-linux-steam");
    var client = Path.Combine(steam, "ubuntu12_32", "steamapps", "content", "app_" + build.AppId);
    foreach (var file in build.Files)
    {
        var target = Path.Combine(client, "depot_" + file.Depot, file.Path);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(Path.Combine(source, file.Path), target);
    }
    Check(SteamDepotDownload.IsComplete(steam, build) && SteamDepotDownload.Locate(steam, build, build.Files[0]).StartsWith(client, StringComparison.Ordinal),
        "The download under ubuntu12_32 was not found.");
    var destination = Path.Combine(root, "depot-download-linux-install", "game");
    GameCopier.CopyFromDepots(steam, destination, build);
    Check(build.Verify(destination).Count == 0, "The copy from the ubuntu12_32 download is not the build.");
    SteamDepotDownload.Delete(steam, build);
    Check(!Directory.Exists(client) && !SteamDepotDownload.IsComplete(steam, build), "The ubuntu12_32 download was not deleted.");
});

Test("Migration copies profiles, settings and game user data into an installation without overwriting", () =>
{
    var old = Path.Combine(root, "migration", "host-startup");
    var appData = Path.Combine(root, "migration", "appdata");
    void Write(string path, string text) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, text); }
    Write(Path.Combine(old, "local-profile", "profile.json"), "legacy progress");
    Write(Path.Combine(old, "local-profile", "profile.lock"), "");
    Write(Path.Combine(old, "profiles", "pp-real", "profile.json"), "main progress");
    Write(Path.Combine(old, "profiles", "Not Valid", "profile.json"), "ignored");
    Write(Path.Combine(old, "launcher-settings.json"), "{\"selectedProfile\":\"pp-real\"}");
    Write(Path.Combine(appData, "user_settings.config"), "shared settings");
    Write(Path.Combine(appData, "saves", "player_user_settings.config"), "shared save");
    Write(Path.Combine(appData, "profiles", "pp-real", "user_settings.config"), "profile settings");
    Write(Path.Combine(appData, "shader_cache", "cache.bin"), "shaders");
    var install = new InstallLayout(Path.Combine(root, "migration", "install"));
    install.Create("12345");
    var result = Migration.Run(install, old, appData);
    Check(File.ReadAllText(Path.Combine(install.Profiles, "default", "profile.json")) == "legacy progress", "Legacy profile not migrated as default.");
    Check(!File.Exists(Path.Combine(install.Profiles, "default", "profile.lock")), "A lock file was copied.");
    Check(File.ReadAllText(Path.Combine(install.Profiles, "pp-real", "profile.json")) == "main progress", "Profile not migrated.");
    Check(!Directory.Exists(Path.Combine(install.Profiles, "Not Valid")), "An invalid profile name was migrated.");
    Check(File.Exists(Path.Combine(install.Root, "launcher-settings.json")), "Launcher settings not migrated.");
    Check(File.ReadAllText(Path.Combine(install.GameProfile("default"), "saves", "player_user_settings.config")) == "shared save" &&
        File.ReadAllText(Path.Combine(install.GameProfile("default"), "user_settings.config")) == "shared settings", "Shared game data not given to default.");
    Check(File.ReadAllText(Path.Combine(install.GameProfile("pp-real"), "user_settings.config")) == "profile settings", "Profile game data not migrated.");
    Check(File.Exists(Path.Combine(install.UserData, "shader_cache", "cache.bin")), "Shader cache not migrated.");
    Check(result.Copied.Count == 6 && result.Kept.Count == 0, $"Unexpected copy count {result.Copied.Count}/{result.Kept.Count}.");
    File.WriteAllText(Path.Combine(install.Profiles, "pp-real", "profile.json"), "played in the installation");
    var again = Migration.Run(install, old, appData);
    Check(again.Copied.Count == 0 && again.Kept.Count == 6, "A second migration copied again.");
    Check(File.ReadAllText(Path.Combine(install.Profiles, "pp-real", "profile.json")) == "played in the installation", "Migration overwrote newer data.");
    Check(File.Exists(Path.Combine(old, "profiles", "pp-real", "profile.json")) && File.Exists(Path.Combine(appData, "shader_cache", "cache.bin")),
        "Migration removed the originals.");
});

Test("Session settings point every game path into the installation, per profile", () =>
{
    var original = "\tlog = \"%APPDATA%/Arrowhead/Helldivers2/network_logs/x.ndp\"\r\n\tuser_settings = \"%APPDATA%/Arrowhead/Helldivers2/user_settings.config\"\r\n" +
        "\tsave_dir = \"%APPDATA%/Arrowhead/Helldivers2/saves\"\r\n";
    var userData = Path.Combine(root, "settings install", "userdata");
    var rewritten = GameSettings.Rewrite(original, userData, "pp-real");
    var prefix = LocalInstall.FullPath(userData).Replace('\\', '/');
    Check(!rewritten.Contains("%APPDATA%") && rewritten.Contains($"log = \"{prefix}/network_logs/x.ndp\"\r\n") &&
        rewritten.Contains($"user_settings = \"{prefix}/profiles/pp-real/user_settings.config\"") &&
        rewritten.Contains($"save_dir = \"{prefix}/profiles/pp-real/saves\""), "Paths not rewritten as expected.");
    Reject(() => GameSettings.Rewrite("no user data here", userData, "pp-real"));
    Reject(() => GameSettings.Rewrite(original, Path.Combine(root, "100%"), "pp-real"));
    try { GameSettings.Rewrite(original, userData, "Bad Name"); throw new Exception("Accepted an invalid profile name."); }
    catch (ArgumentException) { }
});

Test("A profile imports the normal game's settings again (and key bindings), and the import is taken back", () =>
{
    var install = new InstallLayout(Path.Combine(root, "import install"));
    install.Create(SupportedBuild.Current.BuildId);
    var vanilla = Directory.CreateDirectory(Path.Combine(root, "import vanilla", "saves")).Parent!.FullName;
    File.WriteAllText(Path.Combine(vanilla, "user_settings.config"), "game v2");
    File.WriteAllText(Path.Combine(vanilla, "saves", "1100001_user_settings.config"), "account v2");
    File.WriteAllText(Path.Combine(vanilla, "saves", "1100001_user_settings.config.old"), "the game's own backup");
    var controls = Path.Combine(root, "import vanilla", "input_settings.config");
    File.WriteAllText(controls, "keys v2");
    var profile = install.GameProfile("pp-real");
    Directory.CreateDirectory(Path.Combine(profile, "saves"));
    File.WriteAllText(Path.Combine(profile, "user_settings.config"), "game v1");
    File.WriteAllText(Path.Combine(profile, "progress-marker"), "untouched");
    Check(!GameSettings.CanUndoImport(install, "pp-real"), "Nothing to undo before an import.");
    var result = GameSettings.ImportFromGame(install, "pp-real", vanilla, controls, controls: true);
    Check(result.Copied.Count == 3 && result.Missing.Count == 0 && File.ReadAllText(Path.Combine(profile, "user_settings.config")) == "game v2" &&
        File.ReadAllText(Path.Combine(profile, "saves", "1100001_user_settings.config")) == "account v2" &&
        !File.Exists(Path.Combine(profile, "saves", "1100001_user_settings.config.old")) &&
        File.ReadAllText(Path.Combine(profile, "cloud", "input_settings.config")) == "keys v2", "Options, account options and key bindings copied (not the game's .old backups).");
    Check(File.ReadAllText(Path.Combine(profile, "user_settings.config" + GameSettings.BackupSuffix)) == "game v1" && GameSettings.CanUndoImport(install, "pp-real"),
        "The profile's own options are kept beside them.");
    Check(GameSettings.UndoImport(install, "pp-real") == 3 && File.ReadAllText(Path.Combine(profile, "user_settings.config")) == "game v1" &&
        !File.Exists(Path.Combine(profile, "saves", "1100001_user_settings.config")) && !File.Exists(Path.Combine(profile, "cloud", "input_settings.config")) &&
        File.ReadAllText(Path.Combine(profile, "progress-marker")) == "untouched" && !GameSettings.CanUndoImport(install, "pp-real"),
        "Undo: its own options back, the files the import made gone, nothing else touched.");
    var without = GameSettings.ImportFromGame(install, "pp-real", vanilla, null, controls: false);
    Check(without.Copied.Count == 2 && !File.Exists(Path.Combine(profile, "cloud", "input_settings.config")), "Without key bindings: those stay as they are.");
    var nothing = GameSettings.ImportFromGame(install, "pp-real", Path.Combine(root, "no such game"), null, controls: true);
    Check(nothing.Copied.Count == 0 && nothing.Missing.Count == 2, "No normal game on this PC: nothing copied, and said so.");
    try { GameSettings.ImportFromGame(install, "Bad Name", vanilla, null, false); throw new Exception("Imported into an invalid profile name."); }
    catch (ArgumentException) { }
});

Test("Session logs: only the newest ten earlier sessions stay; other folders are never touched", () =>
{
    var sessions = Directory.CreateDirectory(Path.Combine(root, "prune-sessions")).FullName;
    var start = new DateTime(2026, 10, 18, 12, 0, 0, DateTimeKind.Utc);
    var runs = Enumerable.Range(0, 13).Select(i => Guid.NewGuid().ToString("N")).ToList();
    for (var i = 0; i < runs.Count; i++)
    {
        var run = Directory.CreateDirectory(Path.Combine(sessions, runs[i])).FullName;
        File.WriteAllText(Path.Combine(run, "result.json"), new string('x', 1000));
        Directory.SetCreationTimeUtc(run, start.AddMinutes(i)); /* runs[0] the oldest */
    }
    var other = Directory.CreateDirectory(Path.Combine(sessions, "keep-me")).FullName;
    Directory.SetCreationTimeUtc(other, start.AddDays(-1));
    var pruned = SessionLogs.Prune(sessions);
    var left = Directory.GetDirectories(sessions).Select(Path.GetFileName).ToHashSet();
    Check(pruned.Removed == 3 && pruned.Bytes == 3000 && pruned.Skipped == 0, $"Three removed: {pruned}");
    Check(runs.Take(3).All(r => !left.Contains(r)) && runs.Skip(3).All(left.Contains) && left.Contains("keep-me"),
        "The three oldest sessions went; the ten newest and the other folder stayed: " + string.Join(", ", left));
    Check(SessionLogs.Prune(sessions).Removed == 0 && SessionLogs.Prune(Path.Combine(root, "no-such-folder")).Removed == 0, "Nothing more to remove.");
});

Test("Launch options are read as Steam takes them, and never set what a session sets itself", () =>
{
    Check(GameLaunchOptions.Parse("  ").Count == 0 && GameLaunchOptions.Parse(null).Count == 0, "Empty options are none.");
    Check(GameLaunchOptions.Parse("--use-d3d11").SequenceEqual(["--use-d3d11"]), "One option.");
    Check(GameLaunchOptions.Parse("%command% --use-d3d11  -x").SequenceEqual(["--use-d3d11", "-x"]), "Steam's %command% form.");
    var spaced = GameLaunchOptions.Parse("--folder \"C:\\Some Folder\\\" -y");
    Check(spaced.SequenceEqual(["--folder", "C:\\Some Folder\\", "-y"]) && GameLaunchOptions.CommandLine(spaced) == "--folder \"C:\\Some Folder\\\\\" -y",
        "Quotes group, and are put back where needed: " + GameLaunchOptions.CommandLine(spaced));
    foreach (var bad in new[] { "--bundle-dir C:\\other", "-x \"--BUNDLE-DIR\" y", "--bundle-dir=C:\\other", "PROTON_LOG=1 %command%", "%command% %command%",
                 "\"open", "--a\n--b", new string('x', GameLaunchOptions.MaxLength + 1) })
        Reject(() => GameLaunchOptions.Parse(bad));
});

ModTests.Register(Test, Check, Reject, root);
StatsTests.Register(Test, Check, Reject, root);
DataBuilderTests.Register(Test, Check, Reject, root);
SdkTests.Register(Test, Check, Reject, root);
LiveEditTests.Register(Test, Check, Reject, root);
ModKitTests.Register(Test, Check, Reject, root);
BundledModsTests.Register(Test, Check, Reject, root);
CosmeticPacksTests.Register(Test, Check, Reject, root);
EditorTests.Register(Test, Check, Reject, root);
SessionLaunchTests.Register(Test, Check, Reject, root);
PrivacyTests.Register(Test, Check, Reject, root);
DownpatchTests.Register(Test, Check, Reject, root);
ReportedIssuesTests.Register(Test, Check, Reject, root);
SessionJobTests.Register(Test, Check, Reject, root);
ModsHoldTests.Register(Test, Check, Reject, root);
NetworkBlockTests.Register(Test, Check, Reject, root);
ModSyncTests.Register(Test, Check, Reject, root);

Console.WriteLine($"{passed} passed; {failed} failed. Synthetic fixtures: {root}");
return failed == 0 ? 0 : 1;
