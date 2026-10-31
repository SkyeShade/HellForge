using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using HellForge.Core;

// Profile > Account sync (InventorySync, SnapshotSeal, ProfileBackups, AccountImportSettings): where snapshots live, how a
// finished capture becomes one only with a seal for this PC, profile backups and the import choices. Synthetic files only.
static class InventorySyncTests
{
    const string Inventory = """{"items":[{"mixId":1,"amount":1,"isDiscovered":true},{"mixId":2,"amount":3,"isDiscovered":false}]}""";
    const string Progression = """{"experience":485592,"onboardingState":128,"onboardingFlags":7}""";
    const ulong Account = 76561198000000001;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Sync Inventory keeps each profile's snapshot in account-snapshot beside profiles, and an inventory alone makes one", () =>
        {
            var install = Path.Combine(root, "sync-layout");
            var expected = Path.Combine(install, InventorySync.SnapshotFolder, "player");
            check(InventorySync.SnapshotDirectoryFromProfileDir(Path.Combine(install, "profiles", "player")) == expected, "Profile folder.");
            check(InventorySync.SnapshotDirectoryFromProfileDir(Path.Combine(install, "profiles", "player") + Path.DirectorySeparatorChar) == expected, "Trailing separator.");
            check(InventorySync.SnapshotDirectory(install, "player") == expected, "Launcher and backend disagree on the folder.");
            check(InventorySync.SealKeyFor(expected) == Path.Combine(install, InventorySync.SnapshotFolder, SnapshotSeal.KeyFileName), "The seal key is not shared by the installation.");
            check(!InventorySync.HasSnapshot(expected), "A missing folder is a snapshot.");
            Directory.CreateDirectory(expected);
            File.WriteAllText(Path.Combine(expected, InventorySync.InventoryFile), Inventory);
            check(InventorySync.HasSnapshot(expected), "An inventory without progression is not a snapshot.");
        });

        test("Sync Inventory finds its capture only in the platform's installation folder", () =>
        {
            var install = Path.Combine(root, "sync-proxy");
            check(InventorySync.FindCaptureProxy(null) == null, "Found a capture without an installation.");
            check(InventorySync.FindCaptureProxy(install) == null, "Found a capture that is not there.");
            var nativeLinux = OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine;
            var proxy = nativeLinux
                ? Path.Combine(install, "app", "capture-linux", InventorySync.LinuxProxyExe)
                : Path.Combine(install, "app", "capture", InventorySync.ProxyExe);
            Directory.CreateDirectory(Path.GetDirectoryName(proxy)!);
            File.WriteAllText(proxy, "synthetic");
            check(InventorySync.FindCaptureProxy(install) == proxy, "The installed capture was not found.");

            var other = nativeLinux
                ? Path.Combine(install, "app", "capture", InventorySync.ProxyExe)
                : Path.Combine(install, "app", "capture-linux", InventorySync.LinuxProxyExe);
            Directory.CreateDirectory(Path.GetDirectoryName(other)!);
            File.WriteAllText(other, "synthetic");
            check(InventorySync.FindCaptureProxy(install) == proxy, "The capture for the other platform was chosen.");
        });

        test("The capture's hosts line never joins the player's last entry, and cleanup removes only the capture's lines", () => Hosts(root, check));

        test("Linux capture finds HELLDIVERS 2's prefix, maps its certificate to Wine paths, and records it for removal", () =>
            LinuxPaths(root, check));

        test("The Linux supervisor refuses a running game or session, a missing helper, prefix, Proton or API address", () =>
            LinuxRefusals(root, check));

        test("The Linux helper check compares the installed helper and relay with the shipped copies and their hashes", () =>
            InstalledCopies(root, check));

        test("The Linux CA is the Windows Root store's Blob, proved from a UTF-16 LE reg export and system.reg", () =>
            CertificateRegistry(root, check));

        test("The Linux capture refuses a live prefix wineserver and a record from another game library", () =>
            LinuxPrefixChecks(root, check));

        test("The capture refuses a loopback, unspecified, link-local or private API address", () => Addresses(check));

        test("Steam's compatibility-tool mapping chooses HELLDIVERS 2's Proton, or the capture refuses", () => CompatibilityTool(root, check));

        test("Without a CompatToolMapping, the prefix's config_info chooses the Proton that last opened it", () =>
            ConfigInfoProton(root, check));

        test("The launcher installs the capture helper through pkexec and reports cancel, denial and fallbacks", () =>
            HelperInstaller(root, check));

        test("allow-capture.sh names the pkexec caller from PKEXEC_UID", () => HelperInstallerUser(check));

        test("The capture helper writes and removes only its exact hosts lines against a test file", () => HelperHosts(root, check));

        test("The capture helper holds its lock and ends when the watched proxy pid is gone or its lifetime ends", () => HelperWatch(root, check));

        test("A snapshot seal holds only for its files, its Steam account and this installation's key", () =>
            Seals(root, check));

        test("A capture becomes the snapshot only sealed and with an inventory list; a capture without progression never replaces one with it", () =>
            Finalize(root, check));

        test("A Linux seal is bound to the machine id, and a changed key or another machine id fails", () =>
            LinuxMachineBinding(root, check));

        test("A Linux seal fails closed without a valid machine id and refuses a DPAPI-marked key", () =>
            LinuxMachineIdRefusals(root, check));

        test("The capture's state says when it listens and when its files are sealed, for the launcher to take while the game plays on", () =>
            State(root, check));

        test("The capture logs a refused request's short fields, never a token-like value, and only names of headers and query parameters", () =>
            Log(check));

        test("The capture's own reads repeat the game's client headers, without the login's own, so the API's language check passes", () =>
            Headers(check));

        test("The capture's diagnostics give shapes, causes and a tally without values, and a sync collects the game's anti-cheat and crash logs", () =>
            Diagnostics(root, check));

        test("Steam's 32-bit account id is the SteamID64 of an individual account", () =>
            check(SteamAccount.FromAccountId(39734273) == 76561198000000001, "Wrong SteamID64."));

        test("Profile backups are kept newest first, ten at most, and only profile copies count", () => Backups(root, check));

        test("Import choices default per category, keep what the file sets and refuse rules a category does not offer", () => Settings(root, check));
    }

    static void Hosts(string root, Action<bool, string> check)
    {
        const string Host = "api.example.test";
        var folder = Directory.CreateDirectory(Path.Combine(root, "hosts")).FullName;
        // Notepad leaves no newline after the last entry; an empty file and one ending in a newline too.
        foreach (var (name, original) in new[] { ("no-newline", "# player\r\n0.0.0.0 foo.com"), ("newline", "0.0.0.0 foo.com\r\n"), ("empty", "") })
        {
            var hosts = Path.Combine(folder, name);
            File.WriteAllText(hosts, original);
            CaptureHosts.Add(hosts, Host);
            var lines = File.ReadAllLines(hosts);
            check(lines.Contains(CaptureHosts.Marker) && lines.Contains("127.0.0.1 " + Host), name + ": the capture's lines are not lines of their own.");
            check(InventorySync.HasLeftoverRedirect(hosts), name + ": the Linux hosts path did not detect the leftover marker.");
            check(!original.Contains("foo", StringComparison.Ordinal) || lines.Contains("0.0.0.0 foo.com"), name + ": the player's last entry was changed.");
            check(CaptureHosts.Remove(hosts, Host), name + ": cleanup found nothing.");
            check(!InventorySync.HasLeftoverRedirect(hosts), name + ": the Linux hosts path still reports a leftover marker.");
            check(File.ReadAllLines(hosts).SequenceEqual(original.Split("\r\n", StringSplitOptions.RemoveEmptyEntries)), name + ": cleanup changed the player's entries.");
            check(!CaptureHosts.Remove(hosts, Host), name + ": a second cleanup removed more.");
        }
        check(!CaptureHosts.Remove(Path.Combine(folder, "missing"), Host), "Cleanup of a missing file removed something.");
    }

    static void LinuxPaths(string root, Action<bool, string> check)
    {
        var steamRoot = Directory.CreateDirectory(Path.Combine(root, "linux-capture-prefix", "Steam")).FullName;
        var steamApps = Directory.CreateDirectory(Path.Combine(steamRoot, "steamapps")).FullName;
        File.WriteAllText(Path.Combine(steamApps, "appmanifest_553850.acf"), "\"AppState\" { \"appid\" \"553850\" }");
        var prefix = Directory.CreateDirectory(Path.Combine(steamApps, "compatdata", "553850", "pfx")).FullName;
        var compatibilityData = Path.GetDirectoryName(prefix)!;
        check(LinuxCapture.FindPrefix(steamRoot, null) == compatibilityData, "The HELLDIVERS 2 prefix was not found from its Steam app manifest.");

        var sync = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "sync-1")).FullName;
        var caFile = Path.Combine(sync, "capture-ca.cer");
        using var key = RSA.Create(2048);
        var request = new CertificateRequest("CN=HellForge Capture CA", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
        using var certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        var der = certificate.Export(X509ContentType.Cert);
        File.WriteAllBytes(caFile, der);
        var thumbprint = LinuxCapture.CertificateThumbprint(File.ReadAllBytes(caFile));
        check(thumbprint == certificate.Thumbprint && thumbprint.Length == 40, "The public CA blob did not read back as its certificate.");

        var wineCa = LinuxCapture.WinePath(Path.Combine(prefix, "drive_c", "hf-ca.cer"), compatibilityData);
        check(wineCa == @"C:\hf-ca.cer", "A prefix C: certificate was not mapped to C: for Wine: " + wineCa);
        var zCa = LinuxCapture.WinePath(Path.Combine(root, "logs", "capture-ca.cer"), compatibilityData);
        check(zCa.StartsWith(@"Z:\", StringComparison.Ordinal) && zCa.EndsWith(@"\logs\capture-ca.cer", StringComparison.Ordinal),
            "A sync-folder certificate was not mapped through Z: for Wine: " + zCa);

        LinuxCapture.WriteCertificateRecord(sync, new(compatibilityData, Path.Combine(steamRoot, "proton"), steamRoot, caFile, thumbprint));
        var record = LinuxCapture.ReadCertificateRecord(sync);
        check(record is { Thumbprint: var saved } && saved == thumbprint && record.Prefix == compatibilityData && record.CaFile == caFile,
            "The certificate record did not round trip.");
        if (OperatingSystem.IsLinux())
        {
            check(File.GetUnixFileMode(LinuxCapture.CertificateRecordPath(sync)) == (UnixFileMode.UserRead | UnixFileMode.UserWrite),
                "The Linux certificate record is not mode 0600.");

            // A stand-in for `proton run reg import/export`: it copies templates of Wine's system.reg and reg export text,
            // and its reg query prints nothing (the measured behaviour), so only the export file can prove the Blob.
            var fakeFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-proton-root")).FullName;
            var fakeState = Path.Combine(fakeFolder, "root-store.txt");
            var expectedHex = Convert.ToHexString(LinuxCapture.CertificateBlob(der));
            var systemKey = @"Software\\Microsoft\\SystemCertificates\\Root\\Certificates\\" + thumbprint;
            var template = Path.Combine(fakeFolder, "system.reg.template");
            File.WriteAllText(template, "WINE REGISTRY Version 2\n\n[" + systemKey + "]\n\"Blob\"=hex:" + expectedHex + "\n");
            var exportTemplate = Path.Combine(fakeFolder, "export.reg");
            LinuxCapture.WriteRegistryFile(exportTemplate, "Windows Registry Editor Version 5.00\r\n\r\n[" +
                LinuxCapture.RegistryKey(thumbprint) + "]\r\n\"Blob\"=hex:" + expectedHex + "\r\n");
            var fakeProton = WriteFakeProton(fakeFolder, fakeState, template, expectedHex, importLands: true,
                exportTemplate: exportTemplate);
            var fakeRecord = record! with { Proton = fakeProton };
            LinuxCapture.InstallCertificate(fakeRecord);
            check(File.ReadAllText(fakeState).Contains(expectedHex, StringComparison.OrdinalIgnoreCase),
                "The certificate add was not confirmed from the prefix registry value.");
            check(File.ReadAllText(fakeState + ".wineserver").Trim() == "-w",
                "The prefix's own wineserver was not asked to flush after the import.");
            var captureLog = File.ReadAllText(Path.Combine(sync, InventorySync.LauncherLogFile));
            check(captureLog.Contains("Wine reg import: exit 0", StringComparison.Ordinal) &&
                captureLog.Contains("Wine reg export Blob: exit 0", StringComparison.Ordinal) &&
                captureLog.Contains("stdout:", StringComparison.Ordinal) && captureLog.Contains("stderr:", StringComparison.Ordinal) &&
                captureLog.Contains("OpenXR-Loader", StringComparison.Ordinal) &&
                captureLog.Contains("[known Proton noise]", StringComparison.Ordinal),
                "The capture log did not keep both reg streams with Proton's noise marked.");
            check(captureLog.Contains("reg export check: the export file holds the expected Blob", StringComparison.Ordinal) &&
                captureLog.Contains("system.reg check: the key is in system.reg with the expected Blob", StringComparison.Ordinal),
                "The capture log did not record both registry checks.");
            check(LinuxCapture.RemoveCertificate(fakeRecord) == null && File.ReadAllText(fakeState).Length == 0,
                "The certificate key was not verified as removed.");

            // reg import exits 0 but system.reg never gets the key: the message says the import did not land.
            File.WriteAllText(Path.Combine(prefix, "system.reg"), "WINE REGISTRY Version 2\n\n[Other\\\\Key]\n");
            var noImportFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-no-import")).FullName;
            var noImport = WriteFakeProton(noImportFolder, Path.Combine(noImportFolder, "state"), null, expectedHex,
                importLands: false, exportTemplate: null);
            var notLanded = Refusal(() => LinuxCapture.InstallCertificate(fakeRecord with { Proton = noImport }));
            check(notLanded.Contains("the key is not in system.reg: the import did not land", StringComparison.Ordinal) &&
                notLanded.Contains("reg export exited 1", StringComparison.Ordinal) &&
                !notLanded.Contains("ERROR: Unable", StringComparison.Ordinal) &&
                !notLanded.Contains("xrizer", StringComparison.Ordinal),
                "A missing import was not named from the export file and system.reg: " + notLanded);
            var missingLog = File.ReadAllText(Path.Combine(sync, InventorySync.LauncherLogFile));
            check(missingLog.Contains("reg export check: reg export exited 1", StringComparison.Ordinal) &&
                missingLog.Contains("system.reg check: the key is not in system.reg: the import did not land", StringComparison.Ordinal),
                "The capture log did not record both checks for a missing import: " + string.Join(" / ", missingLog.Split('\n').TakeLast(8)));

            // system.reg has the key but reg export writes no file: the message says which check failed.
            var notReadFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-not-read")).FullName;
            var notRead = WriteFakeProton(notReadFolder, Path.Combine(notReadFolder, "state"), template, expectedHex,
                importLands: true, exportTemplate: null);
            var exportLost = Refusal(() => LinuxCapture.InstallCertificate(fakeRecord with { Proton = notRead }));
            check(exportLost.Contains("reg export exited 1", StringComparison.Ordinal) &&
                exportLost.Contains("system.reg holds the expected Blob", StringComparison.Ordinal) &&
                !exportLost.Contains("reg query", StringComparison.Ordinal) &&
                !exportLost.Contains("xrizer", StringComparison.Ordinal),
                "A readable system.reg with a failed export was not named: " + exportLost);

            var badFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-bad-proton")).FullName;
            var badExport = Path.Combine(badFolder, "bad-export.reg");
            LinuxCapture.WriteRegistryFile(badExport, "Windows Registry Editor Version 5.00\r\n\r\n[" +
                LinuxCapture.RegistryKey(thumbprint) + "]\r\n\"Blob\"=hex:00" + expectedHex[2..] + "\r\n");
            var badProton = WriteFakeProton(badFolder, Path.Combine(badFolder, "state"), template, expectedHex,
                importLands: true, exportTemplate: badExport);
            var exportMismatch = Refusal(() => LinuxCapture.InstallCertificate(fakeRecord with { Proton = badProton }));
            check(exportMismatch.Contains("the export file's Blob does not match the certificate", StringComparison.Ordinal) &&
                exportMismatch.Contains("system.reg holds the expected Blob", StringComparison.Ordinal),
                "A mismatching export file was accepted as the capture CA: " + exportMismatch);

            // Removal: reg export fails and writes no file, but system.reg still has the key after the flush. Empty stdout
            // must never be read as gone.
            var lingeringFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-lingering")).FullName;
            var lingering = WriteFakeProton(lingeringFolder, Path.Combine(lingeringFolder, "state"), template, expectedHex,
                importLands: true, exportTemplate: exportTemplate, deleteLands: false);
            File.WriteAllText(Path.Combine(prefix, "system.reg"), File.ReadAllText(template));
            var lingeringProblem = LinuxCapture.RemoveCertificate(fakeRecord with { Proton = lingering });
            check(lingeringProblem != null &&
                lingeringProblem.Contains("reg export did not find the capture CA key", StringComparison.Ordinal) &&
                lingeringProblem.Contains("the key is in system.reg", StringComparison.Ordinal) &&
                lingeringProblem.Contains("not proven gone", StringComparison.Ordinal),
                "A removal with an empty export and a remaining system.reg key was accepted: " + lingeringProblem);

            // Removal on Proton 11: reg export exits 0 for the missing key but writes no file, and system.reg has no key
            // after the flush. That is a proven removal, not a failure retried at every launcher start.
            var exitZeroFolder = Directory.CreateDirectory(Path.Combine(root, "linux-capture-record", "fake-exit-zero")).FullName;
            var exitZero = WriteFakeProton(exitZeroFolder, Path.Combine(exitZeroFolder, "state"), template, expectedHex,
                importLands: true, exportTemplate: exportTemplate, missingExportExit: 0);
            File.WriteAllText(Path.Combine(prefix, "system.reg"), File.ReadAllText(template));
            var exitZeroProblem = LinuxCapture.RemoveCertificate(fakeRecord with { Proton = exitZero });
            check(exitZeroProblem == null && !File.Exists(Path.Combine(prefix, "system.reg")),
                "A removal whose reg export exits 0 without a file was not accepted: " + exitZeroProblem);
            var lingeringZero = WriteFakeProton(exitZeroFolder, Path.Combine(exitZeroFolder, "state"), template, expectedHex,
                importLands: true, exportTemplate: exportTemplate, deleteLands: false, missingExportExit: 0);
            File.WriteAllText(Path.Combine(prefix, "system.reg"), File.ReadAllText(template));
            check(LinuxCapture.RemoveCertificate(fakeRecord with { Proton = lingeringZero }) is { } zeroLingers &&
                zeroLingers.Contains("not proven gone", StringComparison.Ordinal),
                "A removal whose reg export exits 0 while system.reg keeps the key was accepted.");
        }
        LinuxCapture.DeleteCertificateRecord(sync);
        check(LinuxCapture.ReadCertificateRecord(sync) == null, "The removed certificate record still reads.");
    }

    static void LinuxPrefixChecks(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux prefix checks)"); return; }
        var steamRoot = Directory.CreateDirectory(Path.Combine(root, "linux-prefix-checks", "Steam")).FullName;
        var steamApps = Directory.CreateDirectory(Path.Combine(steamRoot, "steamapps")).FullName;
        File.WriteAllText(Path.Combine(steamApps, "appmanifest_553850.acf"), "\"AppState\" { \"appid\" \"553850\" }");
        var prefix = Directory.CreateDirectory(Path.Combine(steamApps, "compatdata", "553850", "pfx")).FullName;
        var compatibilityData = Path.GetDirectoryName(prefix)!;

        var server = LinuxCapture.WineserverServer(prefix);
        check(server is { } name && name.StartsWith("server-", StringComparison.Ordinal),
            "The prefix's wineserver socket name was not derived from its device and inode.");
        var sockets = LinuxCapture.WineserverSocketPaths(server!, "/run/user/1000", 1000);
        check(sockets.SequenceEqual(["/run/user/1000/wine/" + server + "/socket", "/tmp/.wine-1000/" + server + "/socket"]),
            "The prefix's wineserver socket paths do not match hellforge-offline.sh.");
        check(LinuxCapture.WineserverProblem(compatibilityData, _ => LinuxSteamBridge.SocketState.Listening) is { } listening &&
            listening.Contains("close Steam's game/tools in that prefix first", StringComparison.Ordinal),
            "A live wineserver was not refused with the required action.");
        check(LinuxCapture.WineserverProblem(compatibilityData, _ => LinuxSteamBridge.SocketState.NotListening) == null,
            "A prefix with no wineserver was refused.");
        check(LinuxCapture.WineserverProblem(compatibilityData, _ => LinuxSteamBridge.SocketState.CannotTell) is { } unknown &&
            unknown.Contains("close Steam's game/tools in that prefix first", StringComparison.Ordinal),
            "An unreadable wineserver state did not fail closed.");

        var record = new LinuxCapture.CertificateRecord(compatibilityData, Path.Combine(root, "proton"), steamRoot,
            Path.Combine(root, "capture-ca.cer"), new string('A', 40));
        check(LinuxCapture.PrefixMismatchProblem(record) == null, "The library's own prefix was called a mismatch.");
        var second = Directory.CreateDirectory(Path.Combine(root, "linux-prefix-checks", "second-library", "steamapps",
            "compatdata", "553850", "pfx")).FullName;
        var secondData = Path.GetDirectoryName(second)!;
        check(LinuxCapture.PrefixMismatchProblem(record with { Prefix = secondData }) is { } mismatch &&
            mismatch.Contains("second Steam library", StringComparison.Ordinal),
            "A record from another Steam library's prefix was not named.");
        check(LinuxCapture.PrefixMismatchProblem(record with { Prefix = Path.Combine(root, "not-a-game-prefix") }) is { } shape &&
            shape.Contains("not HELLDIVERS 2", StringComparison.Ordinal),
            "A record with a prefix that is not compatdata/553850 was not refused.");

        var version = Path.Combine(compatibilityData, "version");
        File.WriteAllText(version, "proton-cachyos-11.0-20260506-slr\n");
        check(LinuxCapture.PrefixVersion(compatibilityData) == "proton-cachyos-11.0-20260506-slr",
            "The prefix version file was not read as evidence.");
        check(LinuxCapture.PrefixVersion(Path.Combine(root, "linux-prefix-checks", "missing")) == null,
            "A missing prefix version was invented.");
    }

    static void CertificateRegistry(string root, Action<bool, string> check)
    {
        var der = new byte[] { 0x30, 0x03, 0x02, 0x01, 0x00 };
        var blob = LinuxCapture.CertificateBlob(der);
        check(blob.Length == 17 && blob.AsSpan(0, 4).SequenceEqual(new byte[] { 0x20, 0, 0, 0 }) &&
            blob.AsSpan(4, 4).SequenceEqual(new byte[] { 1, 0, 0, 0 }) && blob.AsSpan(8, 4).SequenceEqual(new byte[] { 5, 0, 0, 0 }) &&
            blob.AsSpan(12).SequenceEqual(der), "The certificate Blob's header fields or DER length are wrong.");

        var thumbprint = new string('A', 40);
        var text = LinuxCapture.AddRegistryText(thumbprint.ToLowerInvariant(), der);
        check(text.StartsWith("Windows Registry Editor Version 5.00\r\n\r\n[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\SystemCertificates\\Root\\Certificates\\" +
            thumbprint + "]\r\n", StringComparison.Ordinal), "The add .reg header or key is wrong.");
        check(text.Contains("\"Blob\"=hex:20,00,00,00,01,00,00,00,05,00,00,00,30,03,02,01,00\r\n", StringComparison.Ordinal),
            "The add .reg Blob hex is wrong.");
        check(!text.Replace("\r\n", "", StringComparison.Ordinal).Contains('\n'), "The add .reg does not use CRLF line ends.");
        var delete = LinuxCapture.DeleteRegistryText(thumbprint);
        check(delete.Contains("[-HKEY_LOCAL_MACHINE\\Software\\Microsoft\\SystemCertificates\\Root\\Certificates\\" + thumbprint + "]",
            StringComparison.Ordinal) && delete.EndsWith("\r\n", StringComparison.Ordinal), "The delete .reg key is wrong.");

        var folder = Directory.CreateDirectory(Path.Combine(root, "linux-registry")).FullName;
        var regPath = Path.Combine(folder, "capture-add.reg");
        LinuxCapture.WriteRegistryFile(regPath, text);
        var bytes = File.ReadAllBytes(regPath);
        check(bytes.Length > 2 && bytes[0] == 0xFF && bytes[1] == 0xFE, "The .reg file is not UTF-16 LE with a BOM.");
        check(Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2) == text, "The .reg file text did not round trip as UTF-16 LE.");
        if (OperatingSystem.IsLinux())
            check(File.GetUnixFileMode(regPath) == (UnixFileMode.UserRead | UnixFileMode.UserWrite), "The .reg file is not mode 0600.");

        check(LinuxCapture.NormalizeThumbprint(thumbprint.ToLowerInvariant()) == thumbprint, "A lower-case thumbprint was not normalised.");
        foreach (var bad in new string?[] { null, "", new string('A', 39), new string('A', 41), new string('G', 40), " " + new string('A', 39) })
            check(LinuxCapture.NormalizeThumbprint(bad) == null, "An invalid thumbprint was accepted: " + (bad ?? "null"));
        var threw = false;
        try { LinuxCapture.RegistryKey("not-a-thumbprint"); } catch (ArgumentException) { threw = true; }
        check(threw, "An invalid thumbprint reached a registry key name.");

        var key = LinuxCapture.RegistryKey(thumbprint);
        check(key == @"HKEY_LOCAL_MACHINE\Software\Microsoft\SystemCertificates\Root\Certificates\" + thumbprint, "The registry key is wrong.");
        check(LinuxCapture.ImportRegistryArguments(@"Z:\tmp\capture.reg").SequenceEqual(["run", "reg", "import", @"Z:\tmp\capture.reg"]),
            "The registry import arguments are wrong.");
        check(LinuxCapture.ExportKeyArguments(thumbprint, @"Z:\tmp\capture-export.reg").SequenceEqual(
            ["run", "reg", "export", key, @"Z:\tmp\capture-export.reg", "/y"]),
            "The registry export arguments are wrong.");

        var expected = Convert.ToHexString(blob);
        var exportText = "Windows Registry Editor Version 5.00\r\n\r\n[" + key + "]\r\n\"Blob\"=hex:" +
            expected[..24] + ",\\\r\n  " + expected[24..] + "\r\n";
        var exportPath = Path.Combine(folder, "capture-export.reg");
        var exportBytes = new byte[] { 0xFF, 0xFE }.Concat(Encoding.Unicode.GetBytes(exportText)).ToArray();
        File.WriteAllBytes(exportPath, exportBytes);
        check(LinuxCapture.ReadRegistryExportBlob(exportPath, thumbprint) == expected,
            "A UTF-16 LE reg export with a continued Blob was not read.");
        check(LinuxCapture.ReadExportBlobHex(exportText, thumbprint) == expected,
            "The reg export text parser did not read the Blob.");
        check(LinuxCapture.ReadRegistryExportBlob(exportPath, new string('B', 40)) == null,
            "Another certificate's export was reported present.");
        File.WriteAllBytes(exportPath, Encoding.Unicode.GetBytes(exportText));
        check(LinuxCapture.ReadRegistryExportBlob(exportPath, thumbprint) == null,
            "A reg export without a UTF-16 LE BOM was accepted.");

        var systemKey = @"Software\\Microsoft\\SystemCertificates\\Root\\Certificates\\" + thumbprint;
        var systemReg = "WINE REGISTRY Version 2\n\n[" + systemKey + "]\n\"Blob\"=hex:" + expected + "\n";
        var fromSystemReg = LinuxCapture.ReadSystemRegCertificate(systemReg, thumbprint);
        check(fromSystemReg.KeyFound && fromSystemReg.BlobHex == expected,
            "Wine's system.reg text did not yield the certificate key and Blob.");
        var continued = LinuxCapture.ReadSystemRegCertificate(
            "WINE REGISTRY Version 2\n\n[" + systemKey + "]\n\"Blob\"=hex:" + expected[..24] + ",\\\n  " + expected[24..] + "\n\n[Other\\\\Key]\n",
            thumbprint);
        check(continued.KeyFound && continued.BlobHex == expected, "A continued system.reg Blob was not read in full.");
        check(!LinuxCapture.ReadSystemRegCertificate(systemReg, new string('B', 40)).KeyFound,
            "Another certificate's key was reported present.");
        check(LinuxCapture.ReadSystemRegCertificate("[" + systemKey + "]\n\"Other\"=hex:00\n", thumbprint) is
            { KeyFound: true, BlobHex: null }, "A key without a Blob value was called a missing key.");
        check(LinuxCapture.ReadSystemRegCertificate("[" + systemKey + "]\n\"Blob\"=hex:00\n", thumbprint) is
            { KeyFound: true, BlobHex: "00" }, "A present key with a Blob was misread.");

        check(LinuxCapture.ReadExportBlobHex("Windows Registry Editor Version 5.00\r\n\r\n[Other\\\\Key]\r\n", thumbprint) == null,
            "An export without the certificate key was reported present.");

        var hashFile = Path.Combine(folder, "hash.txt");
        File.WriteAllText(hashFile, "abc");
        check(LinuxCapture.FileSha256(hashFile) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
            "The helper hash is not SHA-256.");
    }

    static void Addresses(Action<bool, string> check)
    {
        foreach (var rejected in new[] { "0.0.0.0", "127.0.0.1", "10.0.0.1", "172.16.0.1", "172.31.255.255",
            "192.168.1.1", "169.254.1.1", "100.64.0.1" })
            check(!LinuxCapture.IsPublicAddress(IPAddress.Parse(rejected)), "A non-public API address was accepted: " + rejected);
        check(LinuxCapture.IsPublicAddress(IPAddress.Parse("8.8.8.8")), "A public API address was rejected.");
        check(!LinuxCapture.IsPublicAddress(IPAddress.IPv6Loopback), "An IPv6 loopback address was accepted as an IPv4 API address.");
        var publicAddress = LinuxCapture.FirstPublicIPv4([IPAddress.Loopback, IPAddress.Parse("10.1.1.1"), IPAddress.Parse("8.8.8.8")]);
        check(publicAddress?.ToString() == "8.8.8.8" && LinuxCapture.FirstPublicIPv4([IPAddress.Loopback]) == null,
            "The first public IPv4 address was not chosen.");
    }

    static void CompatibilityTool(string root, Action<bool, string> check)
    {
        var steam = Directory.CreateDirectory(Path.Combine(root, "compat-tool", "Steam")).FullName;
        var common = Directory.CreateDirectory(Path.Combine(steam, "steamapps", "common")).FullName;
        var proton9 = Directory.CreateDirectory(Path.Combine(common, "Proton 9.0")).FullName;
        var experimental = Directory.CreateDirectory(Path.Combine(common, "Proton - Experimental")).FullName;
        File.WriteAllText(Path.Combine(proton9, "proton"), "");
        File.WriteAllText(Path.Combine(experimental, "proton"), "");
        var config = Directory.CreateDirectory(Path.Combine(steam, "config")).FullName;
        var configPath = Path.Combine(config, "config.vdf");

        const string GameMapping = """
            "InstallConfigStore"
            {
              "Software"
              {
                "Valve"
                {
                  "Steam"
                  {
                    "CompatToolMapping"
                    {
                      "553850" { "name" "proton_9" "config" "" "Priority" "250" }
                      "0" { "name" "proton_experimental" "config" "" }
                    }
                  }
                }
              }
            }
            """;
        check(LinuxCapture.CompatToolName(GameMapping) == "proton_9", "The game's CompatToolMapping name was not read.");
        File.WriteAllText(configPath, GameMapping);
        check(LinuxCapture.ProtonForGame(steam) == Path.Combine(proton9, "proton"),
            "The Proton Steam maps to HELLDIVERS 2 was not chosen.");

        const string DefaultMapping = """
            "InstallConfigStore"
            {
              "Steam"
              {
                "CompatToolMapping"
                {
                  "0" { "name" "proton_experimental" }
                }
              }
            }
            """;
        check(LinuxCapture.CompatToolName(DefaultMapping) == "proton_experimental", "Steam's default CompatToolMapping was not read.");
        File.WriteAllText(configPath, DefaultMapping);
        check(LinuxCapture.ProtonForGame(steam) == Path.Combine(experimental, "proton"),
            "Steam's default Proton was not used when the game has no mapping.");

        const string BrokenMapping = """
            "InstallConfigStore"
            {
              "Steam"
              {
                "CompatToolMapping"
                {
                  "553850" { "config" "" }
                  "0" { "name" "proton_experimental" }
                }
              }
            }
            """;
        check(LinuxCapture.CompatToolName(BrokenMapping) == null, "A game mapping without a name fell back to another Proton.");
        File.WriteAllText(configPath, GameMapping.Replace("proton_9", "proton_not_installed", StringComparison.Ordinal));
        check(LinuxCapture.ProtonForGame(steam) == null, "An unresolved compatibility tool was guessed instead of refused.");
        check(LinuxCapture.ProtonForGame(Path.Combine(root, "missing-steam")) == null, "A missing Steam config resolved a Proton.");
        check(LinuxCapture.CompatToolName("{") == null, "Malformed Steam metadata resolved a tool.");

        var custom = Directory.CreateDirectory(Path.Combine(steam, "compatibilitytools.d", "GE-Proton10-25")).FullName;
        File.WriteAllText(Path.Combine(custom, "proton"), "");
        File.WriteAllText(Path.Combine(custom, "compatibilitytool.vdf"), """
            "compatibilitytools"
            {
              "compat_tools"
              {
                "GE-Proton10-25"
                {
                  "install_path" "."
                  "display_name" "GE-Proton10-25"
                }
              }
            }
            """);
        File.WriteAllText(configPath, GameMapping.Replace("proton_9", "GE-Proton10-25", StringComparison.Ordinal));
        check(LinuxCapture.ProtonForGame(steam) == Path.Combine(custom, "proton"),
            "A compatibilitytools.d Proton was not resolved through its compatibilitytool.vdf.");
    }

    static void ConfigInfoProton(string root, Action<bool, string> check)
    {
        var steam = Directory.CreateDirectory(Path.Combine(root, "config-info", "Steam")).FullName;
        var steamApps = Directory.CreateDirectory(Path.Combine(steam, "steamapps")).FullName;
        File.WriteAllText(Path.Combine(steamApps, "appmanifest_553850.acf"), "\"AppState\" { \"appid\" \"553850\" }");
        var pfx = Directory.CreateDirectory(Path.Combine(steamApps, "compatdata", "553850", "pfx")).FullName;
        var compatibilityData = Path.GetDirectoryName(pfx)!;
        var common = Directory.CreateDirectory(Path.Combine(steamApps, "common")).FullName;
        var mapped = Directory.CreateDirectory(Path.Combine(common, "Proton 9.0")).FullName;
        var info = Directory.CreateDirectory(Path.Combine(common, "Proton - Experimental")).FullName;
        var mappedScript = Path.Combine(mapped, "proton");
        var infoScript = Path.Combine(info, "proton");
        File.WriteAllText(mappedScript, "#!/bin/sh\nexport CURRENT_PREFIX_VERSION=\"11.0-100\"\n");
        File.WriteAllText(infoScript, "#!/bin/sh\nCURRENT_PREFIX_VERSION=\"11.0-100\"\n");
        File.WriteAllText(Path.Combine(compatibilityData, "version"), "11.0-100\n");
        var configInfo = Path.Combine(compatibilityData, "config_info");
        File.WriteAllText(configInfo, "11.0-100\n" + info + "/files/share/fonts/\n" + info + "/files/lib/\n");

        check(LinuxCapture.PrefixVersion(compatibilityData) == "11.0-100", "The prefix version file was not read.");
        check(LinuxCapture.CurrentPrefixVersion(infoScript) == "11.0-100",
            "The Proton script's CURRENT_PREFIX_VERSION was not read.");
        check(LinuxCapture.ProtonForGame(steam) == infoScript,
            "A prefix with no CompatToolMapping did not resolve the Proton from config_info.");
        check(LinuxCapture.FindProtonForGame(steam).Source.Contains("config_info", StringComparison.Ordinal),
            "The Proton resolution did not say it came from config_info.");

        File.WriteAllText(configInfo, "11.0-100\n" + info + "/dist/lib/\n");
        check(LinuxCapture.ProtonForGame(steam) == infoScript, "A dist/ config_info path was not resolved.");

        var config = Directory.CreateDirectory(Path.Combine(steam, "config")).FullName;
        var configPath = Path.Combine(config, "config.vdf");
        File.WriteAllText(configPath, """
            "InstallConfigStore"
            {
              "Software"
              {
                "Valve"
                {
                  "Steam"
                  {
                    "CompatToolMapping"
                    {
                      "553850" { "name" "proton_9" }
                    }
                  }
                }
              }
            }
            """);
        check(LinuxCapture.ProtonForGame(steam) == mappedScript,
            "A CompatToolMapping did not win over the prefix's config_info.");
        File.WriteAllText(configPath, File.ReadAllText(configPath).Replace("proton_9", "proton_missing",
            StringComparison.Ordinal));
        check(LinuxCapture.ProtonForGame(steam) == infoScript,
            "A CompatToolMapping naming a missing tool did not fall back to config_info.");

        File.Delete(configPath);
        File.WriteAllText(Path.Combine(compatibilityData, "version"), "11.0-101\n");
        check(LinuxCapture.ProtonForGame(steam) == null,
            "A config_info Proton whose CURRENT_PREFIX_VERSION differs from the prefix version was accepted.");

        File.WriteAllText(Path.Combine(compatibilityData, "version"), "11.0-100\n");
        File.WriteAllText(configInfo, "11.0-100\n" + Path.Combine(common, "Missing Proton") + "/files/lib/\n");
        var missing = LinuxCapture.FindProtonForGame(steam);
        check(missing.Proton == null && missing.Source.Contains("config_info", StringComparison.Ordinal) &&
            missing.Source.Contains("CompatToolMapping", StringComparison.Ordinal),
            "A config_info naming a missing Proton folder did not refuse with both sources named.");
    }

    static void HelperInstaller(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: pkexec helper install)"); return; }
        var install = Directory.CreateDirectory(Path.Combine(root, "helper-install", "install")).FullName;
        var linuxFolder = Directory.CreateDirectory(Path.Combine(install, "app", "linux")).FullName;
        var installer = Path.Combine(linuxFolder, LinuxCapture.HelperInstaller);
        File.WriteAllText(installer, "#!/bin/sh\n");
        File.SetUnixFileMode(installer, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);

        var description = LinuxCapture.HelperInstallDescription(install);
        check(description.Contains("/usr/local/lib/hellforge", StringComparison.Ordinal) &&
            description.Contains("/etc/sudoers.d/hellforge-capture", StringComparison.Ordinal) &&
            description.Contains("--remove", StringComparison.Ordinal) &&
            description.Contains("pkexec", StringComparison.Ordinal),
            "The helper confirmation does not say what will be installed and how it is removed.");

        var successFolder = Path.Combine(root, "helper-install", "success");
        var argsFile = Path.Combine(successFolder, "args.txt");
        var success = FakePkexec(successFolder, "exit 0", argsFile);
        var installed = LinuxCapture.InstallHelper(install, success, "/usr/bin/bash", _ => null);
        check(installed.Succeeded && File.ReadAllLines(argsFile).SequenceEqual(["/usr/bin/bash", installer]),
            "The launcher did not run the shipped installer with /usr/bin/bash through pkexec: " + installed.Message);
        var removed = LinuxCapture.RemoveHelper(install, success, "/usr/bin/bash", _ => null);
        check(removed.Succeeded && File.ReadAllLines(argsFile).SequenceEqual(["/usr/bin/bash", installer, "--remove"]),
            "The launcher did not run the same installer with --remove through pkexec.");

        var cancel = FakePkexec(Path.Combine(root, "helper-install", "cancel"), "exit 126");
        var cancelled = LinuxCapture.InstallHelper(install, cancel, "/usr/bin/bash", _ => null);
        check(!cancelled.Succeeded && cancelled.Cancelled &&
            cancelled.Message.Contains("not installed; nothing changed", StringComparison.Ordinal),
            "A dismissed password dialog was not reported as nothing changed.");

        var auth = FakePkexec(Path.Combine(root, "helper-install", "auth"),
            "printf '%s\\n' 'Error executing command as another user: Not authorized' >&2\nexit 127");
        var denied = LinuxCapture.InstallHelper(install, auth, "/usr/bin/bash", _ => null);
        check(!denied.Succeeded && denied.Cancelled && denied.Message.Contains("password was not accepted", StringComparison.Ordinal),
            "An authentication failure was not reported as a denial.");

        var noAgent = FakePkexec(Path.Combine(root, "helper-install", "no-agent"),
            "printf '%s\\n' 'Error executing command as another user: No authentication agent found' >&2\nexit 127");
        var manual = LinuxCapture.InstallHelper(install, noAgent, "/usr/bin/bash", _ => null);
        check(!manual.Succeeded && manual.NeedsManual && manual.Message.Contains("No polkit authentication agent", StringComparison.Ordinal) &&
            manual.Message.Contains(installer, StringComparison.Ordinal),
            "A missing polkit agent did not fall back to the terminal command.");

        var missing = Path.Combine(root, "helper-install", "missing-pkexec");
        var noPkexec = LinuxCapture.InstallHelper(install, missing, "/usr/bin/bash", _ => null);
        check(!noPkexec.Succeeded && noPkexec.NeedsManual && noPkexec.Message.Contains("pkexec is not installed", StringComparison.Ordinal) &&
            noPkexec.Message.Contains(installer, StringComparison.Ordinal),
            "A missing pkexec did not fall back to the terminal command.");

        // A package update leaves the installed copy stale; the launcher runs the installer again and only continues when
        // its own re-check passes.
        var marker = Path.Combine(root, "helper-install", "reinstalled.marker");
        var reinstall = FakePkexec(Path.Combine(root, "helper-install", "reinstall"),
            "printf '%s\\n' installed > " + ShQuote(marker) + "\nexit 0");
        Func<string, string?> staleUntilReinstalled = _ => File.Exists(marker)
            ? null : "The installed Linux capture helper does not match this HellForge version.";
        check(staleUntilReinstalled(install) != null, "A stale installed helper was not detected.");
        var reinstalled = LinuxCapture.InstallHelper(install, reinstall, "/usr/bin/bash", staleUntilReinstalled);
        check(reinstalled.Succeeded && File.Exists(marker),
            "The reinstall after an update did not continue after the hashes matched again.");

        var osRelease = Path.Combine(root, "helper-install", "os-release");
        File.WriteAllText(osRelease, "ID=steamos\n");
        check(LinuxCapture.IsSteamOs(osRelease) && LinuxCapture.SteamOsPasswordHint(true).Contains("passwd", StringComparison.Ordinal) &&
            LinuxCapture.SteamOsPasswordHint(false).Length == 0,
            "The SteamOS no-password hint was not available or leaked onto other systems.");
    }

    static void HelperInstallerUser(Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: pkexec caller)"); return; }
        var script = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperInstaller));
        if (!File.Exists(script)) { Console.WriteLine("  (skipped: linux/allow-capture.sh is not in the test working directory)"); return; }
        var uid = RunTestProcess("id", ["-u"]).Output.Trim();
        var user = RunTestProcess("id", ["-un"]).Output.Trim();
        check(uid.Length > 0 && user.Length > 0, "The test could not find the current account.");
        var byPkexec = RunTestProcess("bash", [script, "--test-user"], start =>
        {
            start.Environment.Remove("SUDO_USER");
            start.Environment["PKEXEC_UID"] = uid;
        });
        check(byPkexec.ExitCode == 0 && byPkexec.Output.Trim() == user,
            "allow-capture.sh did not resolve the caller from PKEXEC_UID: " + byPkexec.Output);
        var bySudo = RunTestProcess("bash", [script, "--test-user"], start =>
        {
            start.Environment.Remove("PKEXEC_UID");
            start.Environment["SUDO_USER"] = user;
        });
        check(bySudo.ExitCode == 0 && bySudo.Output.Trim() == user,
            "allow-capture.sh did not resolve the caller from SUDO_USER: " + bySudo.Output);
    }

    [System.Runtime.Versioning.SupportedOSPlatform("linux")]
    static string FakePkexec(string folder, string body, string? argsFile = null)
    {
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, "pkexec");
        var script = "#!/bin/sh\n";
        if (argsFile != null) script += "printf '%s\\n' \"$@\" > " + ShQuote(argsFile) + "\n";
        script += body + "\n";
        File.WriteAllText(path, script);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        return path;
    }

    static (int ExitCode, string Output) RunTestProcess(string file, IReadOnlyList<string> arguments,
        Action<ProcessStartInfo>? configure = null)
    {
        var start = new ProcessStartInfo(file)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        configure?.Invoke(start);
        using var process = Process.Start(start) ?? throw new Exception(file + " did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(10000))
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            throw new Exception(file + " did not finish.");
        }
        return (process.ExitCode, (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim());
    }

    static void LinuxRefusals(string root, Action<bool, string> check)
    {
        var install = Path.Combine(root, "linux-refusals");
        var pfx = Directory.CreateDirectory(Path.Combine(install, "compatdata", "553850", "pfx")).FullName;
        var compatibilityData = Path.GetDirectoryName(pfx)!;
        var proton = Path.Combine(install, "proton");
        File.WriteAllText(proton, "#!/bin/sh\n");
        var proxy = Path.Combine(install, "app", "capture-linux", InventorySync.LinuxProxyExe);
        Directory.CreateDirectory(Path.GetDirectoryName(proxy)!);
        File.WriteAllText(proxy, "synthetic");
        var shippedHelper = Path.Combine(install, "app", "linux", LinuxCapture.HelperScript);
        Directory.CreateDirectory(Path.GetDirectoryName(shippedHelper)!);
        File.WriteAllText(shippedHelper, "#!/bin/sh\n");
        var sync = Path.Combine(install, "logs", "sync-refusal");
        var snapshot = Path.Combine(install, InventorySync.SnapshotFolder, "player");

        check(LinuxCapture.StartRefusal(install, null, null, null, gameRunning: true, sessionRunning: false, helper: new(true, true))!.Contains("Close HELLDIVERS 2", StringComparison.Ordinal),
            "A running game was not refused.");
        check(LinuxCapture.StartRefusal(install, null, null, null, gameRunning: false, sessionRunning: true, helper: new(true, true))!.Contains("session", StringComparison.Ordinal),
            "A running session was not refused.");
        check(LinuxCapture.StartRefusal(install, null, null, null, gameRunning: false, sessionRunning: false, helper: new(false, false))!.Contains("allow-capture.sh", StringComparison.Ordinal),
            "A missing helper did not name its installer.");
        check(LinuxCapture.StartRefusal(install, null, null, null, gameRunning: false, sessionRunning: false, helper: new(true, false))!.Contains("allow-capture.sh", StringComparison.Ordinal),
            "A stale helper did not name its installer.");
        check(LinuxCapture.StartRefusal(install, null, proton, "8.8.8.8", gameRunning: false, sessionRunning: false, helper: new(true, true))!.Contains("prefix", StringComparison.Ordinal),
            "A missing game prefix was not refused.");
        var protonRefusal = LinuxCapture.StartRefusal(install, compatibilityData, null, "8.8.8.8",
            gameRunning: false, sessionRunning: false, helper: new(true, true))!;
        check(protonRefusal.Contains("Proton", StringComparison.Ordinal) &&
            protonRefusal.Contains("CompatToolMapping", StringComparison.Ordinal) &&
            protonRefusal.Contains("config_info", StringComparison.Ordinal) &&
            !protonRefusal.Contains("Properties > Compatibility", StringComparison.Ordinal),
            "A missing Proton did not name both sources and the start-once fix: " + protonRefusal);
        check(LinuxCapture.StartRefusal(install, compatibilityData, Path.Combine(install, "missing-proton"), "8.8.8.8",
                gameRunning: false, sessionRunning: false, helper: new(true, true))!.Contains("Proton", StringComparison.Ordinal),
            "A missing Proton file was not refused.");
        check(LinuxCapture.StartRefusal(install, compatibilityData, proton, null, gameRunning: false, sessionRunning: false, helper: new(true, true))!.Contains("public IPv4", StringComparison.Ordinal),
            "A missing API address did not say it needs a public IPv4 address.");
        check(LinuxCapture.StartRefusal(install, compatibilityData, proton, "127.0.0.1", gameRunning: false, sessionRunning: false, helper: new(true, true))!.Contains("not a public IPv4", StringComparison.Ordinal),
            "A loopback API address was not refused with its reason.");
        check(LinuxCapture.StartRefusal(install, compatibilityData, proton, "192.168.1.2", gameRunning: false, sessionRunning: false, helper: new(true, true))!.Contains("not a public IPv4", StringComparison.Ordinal),
            "A private API address was not refused with its reason.");
        check(LinuxCapture.StartRefusal(install, compatibilityData, proton, "8.8.8.8", gameRunning: false, sessionRunning: false, helper: new(true, true)) == null,
            "A ready capture was refused.");

        // The real caller's own checks: a session flag from the launcher, and the actual game process lookup.
        if (OperatingSystem.IsLinux())
        {
            var session = Refusal(() => InventorySync.StartCaptureProxy(install, proxy, Path.Combine(install, "logs", "sync-session"), snapshot, Account, sessionRunning: true));
            check(session.Contains("session", StringComparison.Ordinal), "StartCaptureProxy did not use the launcher's session check: " + session);

            // A script, not a copy of sleep: a script's process takes the script's name, while a multi-call coreutils
            // (Ubuntu 26.04's) refuses to run as an unknown program. The shell stays while its sleep runs.
            var fakeGame = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "linux-refusals-game")).FullName, "helldivers2");
            File.WriteAllText(fakeGame, "#!/bin/sh\nsleep 30\n");
            File.SetUnixFileMode(fakeGame, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);
            using var game = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(fakeGame)
            {
                UseShellExecute = false,
                CreateNoWindow = true
            }) ?? throw new Exception("The synthetic game did not start.");
            try
            {
                for (var waited = 0; waited < 50 && !InventorySync.IsGameRunning(); waited++) Thread.Sleep(100);
                check(InventorySync.IsGameRunning(), "The synthetic game process was not found.");
                var running = Refusal(() => InventorySync.StartCaptureProxy(install, proxy, sync, snapshot, Account, sessionRunning: false));
                check(running.Contains("Close HELLDIVERS 2", StringComparison.Ordinal), "StartCaptureProxy did not use the real game check: " + running);
                var record = new LinuxCapture.CertificateRecord(compatibilityData, proton, install, Path.Combine(install, "capture-ca.cer"), new string('A', 40));
                check(LinuxCapture.RemoveCertificate(record)?.Contains("Close HELLDIVERS 2", StringComparison.Ordinal) == true,
                    "Certificate removal touched the prefix while the game ran.");
                check(LinuxCapture.Cleanup(install)?.Contains("Close HELLDIVERS 2", StringComparison.Ordinal) == true,
                    "Cleanup touched the prefix while the game ran.");
                var installBlocked = false;
                try { LinuxCapture.InstallCertificate(record); }
                catch (InvalidOperationException error) { installBlocked = error.Message.Contains("Close HELLDIVERS 2", StringComparison.Ordinal); }
                check(installBlocked, "Certificate install touched the prefix while the game ran.");
            }
            finally
            {
                if (!game.HasExited) game.Kill(entireProcessTree: true);
                game.WaitForExit(5000);
            }
        }
    }

    static string Refusal(Action action)
    {
        try { action(); return "no refusal"; }
        catch (InvalidOperationException error) { return error.Message; }
    }

    // The launcher's HelperProblem checks both root copies: the helper script and the relay it runs. InstalledCopyProblem is
    // the shared half, exercised here with temporary files (the installed paths themselves need root).
    static void InstalledCopies(string root, Action<bool, string> check)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "installed-copies")).FullName;
        foreach (var name in new[] { "capture helper", "capture relay" })
        {
            var file = name.Replace(' ', '-');
            var shipped = Path.Combine(folder, file + ".shipped");
            var installed = Path.Combine(folder, file);
            var hash = installed + ".sha256";
            File.WriteAllText(shipped, "current");
            File.WriteAllText(installed, "current");
            File.WriteAllText(hash, LinuxCapture.FileSha256(installed));
            check(LinuxCapture.InstalledCopyProblem(installed, hash, shipped, name, root) == null,
                name + ": a current installed copy was refused.");

            File.WriteAllText(shipped, "new version");
            check(LinuxCapture.InstalledCopyProblem(installed, hash, shipped, name, root)?.Contains(
                "does not match this HellForge version", StringComparison.Ordinal) == true,
                name + ": a stale installed copy was accepted.");

            File.WriteAllText(shipped, "current");
            File.WriteAllText(hash, new string('0', 64));
            check(LinuxCapture.InstalledCopyProblem(installed, hash, shipped, name, root)?.Contains(
                "recorded SHA-256", StringComparison.Ordinal) == true,
                name + ": a wrong recorded hash was accepted.");

            File.Delete(shipped);
            check(LinuxCapture.InstalledCopyProblem(installed, hash, shipped, name, root)?.Contains(
                "is missing", StringComparison.Ordinal) == true,
                name + ": a missing shipped file was accepted.");
        }
    }

    static void HelperHosts(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux capture helper hosts test)"); return; }
        var script = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperScript));
        if (!File.Exists(script)) { Console.WriteLine("  (skipped: linux/capture-helper.sh is not in the test working directory)"); return; }
        var folder = Directory.CreateDirectory(Path.Combine(root, "helper-hosts")).FullName;
        var hosts = Path.Combine(folder, "hosts");
        var state = Path.Combine(folder, "state");
        const string player = "10.0.0.1 a.b # HellForge-CaptureProxy";
        File.WriteAllText(hosts, "127.0.0.1 localhost\n" + player);
        var mode = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead | UnixFileMode.OtherRead;
        File.SetUnixFileMode(hosts, mode);

        var add = RunHelper(script, "hosts-add", "--hosts-file", hosts, "--state-dir", state);
        check(add.ExitCode == 0, "The helper could not add its hosts lines: " + add.Output);
        var lines = File.ReadAllLines(hosts);
        check(lines.Contains(CaptureHosts.Marker) && lines.Contains("127.0.0.1 " + LinuxCapture.ApiHost),
            "The helper's marker or mapping is missing.");
        check(lines.Contains(player), "The player's line that contains the marker text was removed.");
        check(File.GetUnixFileMode(hosts) == mode, "The helper did not keep the hosts file's mode.");
        check(RunHelper(script, "hosts-add", "--hosts-file", hosts, "--state-dir", state).ExitCode == 0,
            "A second hosts add failed.");
        check(File.ReadAllLines(hosts).Count(line => line == CaptureHosts.Marker) == 1,
            "The helper duplicated its marker.");

        var remove = RunHelper(script, "hosts-remove", "--hosts-file", hosts, "--state-dir", state);
        check(remove.ExitCode == 0, "The helper could not remove its hosts lines: " + remove.Output);
        var after = File.ReadAllLines(hosts);
        check(!after.Contains(CaptureHosts.Marker) && !after.Contains("127.0.0.1 " + LinuxCapture.ApiHost),
            "The helper left its marker or mapping behind.");
        check(after.Contains(player), "The helper removed the player's line that merely contains the marker text.");
        var missing = Path.Combine(folder, "missing-hosts");
        check(RunHelper(script, "hosts-remove", "--hosts-file", missing, "--state-dir", state).ExitCode == 0 && !File.Exists(missing),
            "Removing a missing hosts file failed or created it.");
    }

    static void HelperWatch(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux capture helper watch test)"); return; }
        var script = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperScript));
        if (!File.Exists(script)) { Console.WriteLine("  (skipped: linux/capture-helper.sh is not in the test working directory)"); return; }
        var folder = Directory.CreateDirectory(Path.Combine(root, "helper-watch")).FullName;
        var hosts = Path.Combine(folder, "hosts");
        var state = Path.Combine(folder, "state");
        File.WriteAllText(hosts, "# player\n");

        using var standIn = StartProxyStandIn();
        var watch = StartHelperWatch(script, standIn.Id, hosts, state, seconds: null);
        try
        {
            WaitFor(() => File.ReadAllText(hosts).Contains(CaptureHosts.Marker, StringComparison.Ordinal), 5000,
                "The watch helper did not add its hosts lines.");
            var locked = RunHelper(script, "hosts-add", "--hosts-file", hosts, "--state-dir", state);
            check(locked.ExitCode != 0 && locked.Output.Contains("another capture helper", StringComparison.Ordinal),
                "A second helper did not take the same lock: " + locked.Output);
            standIn.Kill();
            check(standIn.WaitForExit(5000), "The watched pid did not end.");
            check(watch.WaitForExit(10000), "The watch helper did not end after its pid was gone.");
            check(!File.ReadAllText(hosts).Contains(CaptureHosts.Marker, StringComparison.Ordinal),
                "The watch helper left its hosts lines after its pid was gone.");
        }
        finally
        {
            if (!watch.HasExited) { try { watch.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } }
            watch.Dispose();
            if (!standIn.HasExited) { try { standIn.Kill(); } catch (InvalidOperationException) { } }
        }

        using var standInWithCap = StartProxyStandIn();
        var capped = StartHelperWatch(script, standInWithCap.Id, hosts, state, seconds: "1");
        try
        {
            WaitFor(() => File.ReadAllText(hosts).Contains(CaptureHosts.Marker, StringComparison.Ordinal), 5000,
                "The capped helper did not add its hosts lines.");
            check(capped.WaitForExit(10000), "The helper's hard lifetime cap did not end it.");
            check(!File.ReadAllText(hosts).Contains(CaptureHosts.Marker, StringComparison.Ordinal),
                "The capped helper left its hosts lines behind.");
        }
        finally
        {
            if (!capped.HasExited) { try { capped.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } }
            capped.Dispose();
            if (!standInWithCap.HasExited) { try { standInWithCap.Kill(); } catch (InvalidOperationException) { } }
        }
    }

    static Process StartProxyStandIn()
    {
        var start = new ProcessStartInfo("bash") { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("-c");
        start.ArgumentList.Add("exec -a HellForge.CaptureProxy sleep 30");
        return Process.Start(start) ?? throw new Exception("The proxy stand-in did not start.");
    }

    static Process StartHelperWatch(string helper, int pid, string hosts, string state, string? seconds)
    {
        var start = new ProcessStartInfo("bash") { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add(helper);
        start.ArgumentList.Add("watch");
        start.ArgumentList.Add("--pid");
        start.ArgumentList.Add(pid.ToString(System.Globalization.CultureInfo.InvariantCulture));
        start.ArgumentList.Add("--hosts-file");
        start.ArgumentList.Add(hosts);
        start.ArgumentList.Add("--state-dir");
        start.ArgumentList.Add(state);
        if (seconds != null)
        {
            start.ArgumentList.Add("--seconds");
            start.ArgumentList.Add(seconds);
        }
        return Process.Start(start) ?? throw new Exception("The watch helper did not start.");
    }

    // A stand-in `proton`: import copies Wine's system.reg template, reg query prints nothing (the measured behaviour),
    // and reg export copies a UTF-16 LE template to the path it is given. deleteLands:false leaves system.reg behind so
    // the removal test can prove that an empty export is not enough. Its sibling files/bin/wineserver records the flush.
    [System.Runtime.Versioning.SupportedOSPlatform("linux")]
    static string WriteFakeProton(string folder, string state, string? template, string expectedHex, bool importLands,
        string? exportTemplate, bool deleteLands = true, int missingExportExit = 1)
    {
        Directory.CreateDirectory(folder);
        var proton = Path.Combine(folder, "proton");
        var wineserver = Path.Combine(folder, "files", "bin", "wineserver");
        Directory.CreateDirectory(Path.GetDirectoryName(wineserver)!);
        File.WriteAllText(wineserver, "#!/bin/sh\nprintf '%s\\n' \"$*\" > " + ShQuote(state + ".wineserver") + "\n");
        var script = "#!/bin/sh\n" +
            "state=" + ShQuote(state) + "\n" +
            (template == null ? "" : "template=" + ShQuote(template) + "\n") +
            (exportTemplate == null ? "export_template=''\n" : "export_template=" + ShQuote(exportTemplate) + "\n") +
            "reg=\"$WINEPREFIX/system.reg\"\n" +
            "case \"$*\" in\n" +
            (deleteLands
                ? "  *'reg import'*capture-delete.reg*) : > \"$state\"; rm -f \"$reg\" ;;\n"
                : "  *'reg import'*capture-delete.reg*) : > \"$state\" ;;\n") +
            (importLands
                ? "  *'reg import'*) printf '%s\\n' " + ShQuote(expectedHex) + " > \"$state\"; cp \"$template\" \"$reg\"; printf 'OpenXR-Loader: xrizer noise\\n' >&2 ;;\n"
                : "  *'reg import'*) printf 'reg import wrote nothing\\n' ;;\n") +
            "  *'reg export'*) out=\"$5\"; case \"$out\" in Z:*) out=\"${out#Z:}\"; out=$(printf '%s' \"$out\" | tr '\\\\' '/') ;; esac; if [ -s \"$state\" ] && [ -n \"$export_template\" ]; then cp \"$export_template\" \"$out\"; else rm -f \"$out\"; exit " + missingExportExit.ToString(System.Globalization.CultureInfo.InvariantCulture) + "; fi ;;\n" +
            "  *'reg query'*) : ;;\n" +
            "esac\n";
        File.WriteAllText(proton, script);
        var mode = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
        File.SetUnixFileMode(proton, mode);
        File.SetUnixFileMode(wineserver, mode);
        return proton;
    }

    static string ShQuote(string value) => "'" + value.Replace("'", "'\\''", StringComparison.Ordinal) + "'";

    static (int ExitCode, string Output) RunHelper(string helper, params string[] arguments)
    {
        var start = new ProcessStartInfo("bash")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add(helper);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new Exception("The capture helper did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(10000))
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            throw new Exception("The capture helper did not finish.");
        }
        return (process.ExitCode, (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim());
    }

    static void WaitFor(Func<bool> condition, int milliseconds, string reason)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(milliseconds);
        while (DateTime.UtcNow < deadline)
        {
            if (condition()) return;
            Thread.Sleep(50);
        }
        throw new Exception(reason);
    }

    static void Seals(string root, Action<bool, string> check)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "seal", "account-snapshot", "player")).FullName;
        var key = InventorySync.SealKeyFor(folder);
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        var captured = new DateTime(2026, 10, 25, 9, 30, 0, DateTimeKind.Utc);
        SnapshotSeal.Write(folder, key, Account, captured);
        check(File.Exists(key) && SnapshotSeal.Verify(folder, key) is { SteamAccount: Account } info && info.CapturedUtc == captured, "A fresh seal does not verify.");

        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression.Replace("485592", "999999", StringComparison.Ordinal));
        check(SnapshotSeal.Verify(folder, key) == null, "Edited progression still verifies.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        File.Delete(Path.Combine(folder, SnapshotSeal.ProgressionFile));
        check(SnapshotSeal.Verify(folder, key) == null, "A seal over progression verifies without it.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        check(SnapshotSeal.Verify(folder, key) != null, "The restored files no longer verify.");

        var otherKey = Path.Combine(root, "seal-other", SnapshotSeal.KeyFileName);
        var other = Directory.CreateDirectory(Path.Combine(root, "seal-other", "player")).FullName;
        File.WriteAllText(Path.Combine(other, SnapshotSeal.InventoryFile), Inventory);
        SnapshotSeal.Write(other, otherKey, Account, captured);
        check(SnapshotSeal.Verify(folder, otherKey) == null && SnapshotSeal.Verify(other, key) == null, "A seal verifies under another installation's key.");
        check(SnapshotSeal.Verify(folder, Path.Combine(root, "seal", "missing.key")) == null, "A seal verifies without a key.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.FileName), "{");
        check(SnapshotSeal.Verify(folder, key) == null, "A broken seal verifies.");
    }

    static void LinuxMachineBinding(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux machine-id seal)"); return; }
        if (SnapshotSeal.MachineData.MachineIdPathForTests is not { } machineId)
        {
            Console.WriteLine("  (skipped: no synthetic machine id is set)");
            return;
        }
        var other = MachineIdFile(root, "other", "fedcba9876543210fedcba9876543210\n");
        try
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "seal-linux-machine", "account-snapshot", "player")).FullName;
            var key = InventorySync.SealKeyFor(folder);
            File.WriteAllText(Path.Combine(folder, SnapshotSeal.InventoryFile), Inventory);
            File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
            var captured = new DateTime(2026, 10, 25, 9, 30, 0, DateTimeKind.Utc);
            SnapshotSeal.Write(folder, key, Account, captured);
            check(SnapshotSeal.Verify(folder, key) is { SteamAccount: Account } info && info.CapturedUtc == captured,
                "A fresh Linux seal does not verify.");
            check(File.ReadAllBytes(key).AsSpan().StartsWith("HFL1"u8), "The Linux seal key has no Linux marker.");

            SnapshotSeal.MachineData.MachineIdPathForTests = other;
            check(SnapshotSeal.Verify(folder, key) == null, "A seal verified under another machine id.");
            SnapshotSeal.Write(folder, key, Account, captured);
            check(SnapshotSeal.Verify(folder, key) is { SteamAccount: Account }, "A key from another machine id was not replaced on Write.");

            var blob = File.ReadAllBytes(key);
            blob[^1] ^= 0x80;
            File.WriteAllBytes(key, blob);
            check(SnapshotSeal.Verify(folder, key) == null, "A tampered Linux seal key verified.");
        }
        finally { SnapshotSeal.MachineData.MachineIdPathForTests = machineId; }
    }

    static void LinuxMachineIdRefusals(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux machine-id seal)"); return; }
        if (SnapshotSeal.MachineData.MachineIdPathForTests is not { } machineId)
        {
            Console.WriteLine("  (skipped: no synthetic machine id is set)");
            return;
        }
        const string OtherSystem = "This snapshot was sealed on another system: sync again";
        try
        {
            foreach (var (name, id) in new (string Name, string? Id)[] { ("missing", null), ("short", "abc\n") })
            {
                var badId = MachineIdFile(root, name, id);
                SnapshotSeal.MachineData.MachineIdPathForTests = badId;
                var folder = Directory.CreateDirectory(Path.Combine(root, "seal-no-machine", name)).FullName;
                var key = Path.Combine(folder, SnapshotSeal.KeyFileName);
                File.WriteAllText(Path.Combine(folder, SnapshotSeal.InventoryFile), Inventory);
                var reason = "";
                try { SnapshotSeal.Write(folder, key, Account, DateTime.UtcNow); }
                catch (CryptographicException error) { reason = error.Message; }
                check(reason.Contains("machine id", StringComparison.OrdinalIgnoreCase) && !File.Exists(key),
                    name + ": a missing or short machine id did not fail closed with its reason.");

                SnapshotSeal.MachineData.MachineIdPathForTests = machineId;
                SnapshotSeal.Write(folder, key, Account, DateTime.UtcNow);
                SnapshotSeal.MachineData.MachineIdPathForTests = badId;
                check(SnapshotSeal.Verify(folder, key, out var verifyProblem) == null && verifyProblem is { } &&
                    verifyProblem.Contains("machine id", StringComparison.OrdinalIgnoreCase),
                    name + ": verification did not fail closed with the machine-id reason.");

                var before = File.ReadAllBytes(key);
                var blocked = "";
                try { SnapshotSeal.Write(folder, key, Account, DateTime.UtcNow); }
                catch (CryptographicException error) { blocked = error.Message; }
                check(blocked.Contains("machine id", StringComparison.OrdinalIgnoreCase) && File.ReadAllBytes(key).SequenceEqual(before),
                    name + ": Write replaced a key without a usable machine id.");
            }

            SnapshotSeal.MachineData.MachineIdPathForTests = machineId;
            var sync = Directory.CreateDirectory(Path.Combine(root, "seal-dpapi", "sync")).FullName;
            var snapshot = Path.Combine(root, "seal-dpapi", "account-snapshot", "player");
            var keyPath = InventorySync.SealKeyFor(snapshot);
            File.WriteAllText(Path.Combine(sync, SnapshotSeal.InventoryFile), Inventory);
            File.WriteAllText(Path.Combine(sync, SnapshotSeal.ProgressionFile), Progression);
            SnapshotSeal.Write(sync, keyPath, Account, DateTime.UtcNow);

            var dpapi = new byte[64];
            dpapi[0] = 1; // DPAPI blobs start 01 00 00 00, never the Linux HFL1 marker.
            File.WriteAllBytes(keyPath, dpapi);
            check(SnapshotSeal.Verify(sync, keyPath, out var problem) == null && problem == OtherSystem,
                "A DPAPI-marked key was not refused on Linux with the foreign-seal reason.");
            check(InventorySync.FinalizeCapture(sync, snapshot) == OtherSystem,
                "FinalizeCapture did not pass on the foreign-seal reason.");

            SnapshotSeal.Write(sync, keyPath, Account, DateTime.UtcNow);
            check(File.ReadAllBytes(keyPath).AsSpan().StartsWith("HFL1"u8) && SnapshotSeal.Verify(sync, keyPath) is { SteamAccount: Account },
                "A DPAPI-marked key was not replaced on Write.");
        }
        finally { SnapshotSeal.MachineData.MachineIdPathForTests = machineId; }
    }

    static string MachineIdFile(string root, string name, string? id)
    {
        var path = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "machine-ids")).FullName, name);
        if (id == null) File.Delete(path);
        else File.WriteAllText(path, id);
        return path;
    }

    static void Finalize(string root, Action<bool, string> check)
    {
        var sync = Path.Combine(root, "sync-run");
        var snapshot = Path.Combine(root, "sync-install", InventorySync.SnapshotFolder, "player");
        var key = InventorySync.SealKeyFor(snapshot);
        Directory.CreateDirectory(sync);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null, "A capture without an inventory was saved.");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), """{"error":"denied"}""");
        check(InventorySync.FinalizeCapture(sync, snapshot) != null, "An inventory without items was saved.");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), Progression);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && !Directory.Exists(snapshot), "An unsealed capture was saved.");

        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) == null, "A sealed capture was refused.");
        check(!File.Exists(Path.Combine(sync, InventorySync.InventoryFile)) && !File.Exists(Path.Combine(sync, SnapshotSeal.FileName)), "The capture's files stayed in the sync folder.");
        check(SnapshotSeal.Verify(snapshot, key) is { SteamAccount: Account }, "The moved snapshot no longer verifies.");
        check(InventorySync.ReadMetadata(snapshot) is { Items: 2, Experience: 485592, Progression: true, CapturedUtc: not null }, "Metadata does not describe the capture.");

        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), "[1]");
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && File.Exists(Path.Combine(snapshot, InventorySync.ProgressionFile)),
            "A progression that is not an object was saved, or changed the snapshot.");
        File.Delete(Path.Combine(sync, InventorySync.ProgressionFile));
        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && File.Exists(Path.Combine(snapshot, InventorySync.ProgressionFile)) &&
            InventorySync.ReadMetadata(snapshot) is { Items: 2, Experience: 485592, Progression: true },
            "An inventory-only capture replaced the complete snapshot, or was not refused.");
        check(File.Exists(Path.Combine(sync, InventorySync.InventoryFile)), "The refused capture's files were moved.");

        // A progression without XP is still progression; a snapshot from before the flag has it exactly when it has XP.
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), """{"onboardingState":128}""");
        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) == null && InventorySync.ReadMetadata(snapshot) is { Experience: null, Progression: true },
            "A progression without XP does not count as progression.");

        // With no complete snapshot to keep, an inventory-only capture still makes one: the backend's premium ownership
        // comes from it, while the launcher reports that sync as failed.
        var newcomer = Path.Combine(root, "sync-install", InventorySync.SnapshotFolder, "newcomer");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.Delete(Path.Combine(sync, InventorySync.ProgressionFile));
        SnapshotSeal.Write(sync, InventorySync.SealKeyFor(newcomer), Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, newcomer) == null && InventorySync.ReadMetadata(newcomer) is { Items: 2, Experience: null, Progression: false },
            "An inventory-only capture made no snapshot where there was none.");

        File.WriteAllText(Path.Combine(snapshot, InventorySync.MetadataFile), """{"items":2,"experience":7}""");
        check(InventorySync.ReadMetadata(snapshot) is { Progression: true }, "An older snapshot with XP lost its progression.");
        File.WriteAllText(Path.Combine(snapshot, InventorySync.MetadataFile), """{"items":2,"experience":null}""");
        check(InventorySync.ReadMetadata(snapshot) is { Progression: false }, "An older snapshot without XP claims progression.");
    }

    static void State(string root, Action<bool, string> check)
    {
        var sync = Directory.CreateDirectory(Path.Combine(root, "sync-state")).FullName;
        check(CaptureState.Read(sync) == null && !InventorySync.IsListening(sync) && !InventorySync.IsSealed(sync), "A missing state reads as a capture.");
        CaptureState.Write(sync, 42, sealedCapture: false, progression: false);
        check(InventorySync.IsListening(sync) && !InventorySync.IsSealed(sync), "A listening capture reads as sealed, or not as listening.");
        CaptureState.Write(sync, 42, sealedCapture: true, progression: false);
        check(InventorySync.IsSealed(sync) && CaptureState.Read(sync) is { Progression: false }, "A sealed partial capture reads wrong.");
        File.WriteAllText(Path.Combine(sync, CaptureState.FileName), "{\"sealed\":");
        check(!InventorySync.IsSealed(sync), "A half-written state reads as sealed.");
    }

    static void Diagnostics(string root, Action<bool, string> check)
    {
        static byte[] Utf8(string text) => System.Text.Encoding.UTF8.GetBytes(text);
        var shape = CaptureDiagnostics.JsonShape(Utf8("""{"items":[{"mixId":1},{"mixId":2}],"limitSettings":{"a":1,"b":2},"name":"Secret Ship","xp":5}"""));
        check(shape == "object{items[2], limitSettings{2}, name:string, xp:number}", "The body's shape is wrong: " + shape);
        check(!shape.Contains("Secret", StringComparison.Ordinal) && !shape.Contains('5', StringComparison.Ordinal), "A value reached the shape.");
        check(CaptureDiagnostics.JsonShape(Utf8("[1,2,3]")) == "array[3]" && CaptureDiagnostics.JsonShape([]) == "empty" &&
            CaptureDiagnostics.JsonShape(Utf8("nope")) == "not JSON, 4 bytes", "Shapes of other bodies are wrong.");
        var chain = CaptureDiagnostics.Chain(new HttpRequestException("send failed", new IOException("read failed",
            new System.Net.Sockets.SocketException((int)System.Net.Sockets.SocketError.ConnectionReset))));
        check(chain.StartsWith("HttpRequestException: send failed <- IOException: read failed <- SocketException (10054): ", StringComparison.Ordinal),
            "The cause chain is wrong: " + chain);

        var tally = new CaptureTally();
        tally.Add("POST", "/api/Proxy/...", 200);
        tally.Add("POST", "/api/Proxy/...", 200);
        tally.Add("GET", "/api/Progression", 400);
        check(tally.Total == 3 && tally.Lines().SequenceEqual(["GET /api/Progression 400 x1", "POST /api/Proxy/... 200 x2"]), "The tally is wrong.");

        var sync = Directory.CreateDirectory(Path.Combine(root, "sync-diagnostics")).FullName;
        InventorySync.Note(sync, "first step");
        InventorySync.Note(Path.Combine(root, "no-such-folder", "sync"), "lost");
        check(File.ReadAllText(Path.Combine(sync, InventorySync.LauncherLogFile)).TrimEnd().EndsWith(" first step", StringComparison.Ordinal),
            "The launcher's step was not noted.");

        var appData = Path.Combine(root, "appdata");
        var since = DateTime.UtcNow.AddMinutes(-1);
        check(InventorySync.CollectGameLogs(sync, since, appData).SequenceEqual(["no Easy Anti-Cheat launcher log written since the sync started",
            "no game crash report since the sync started"]), "Missing game logs are not reported as missing.");
        var eacLog = Path.Combine(appData, "EasyAntiCheat", "product", "deployment", "anticheatlauncher.log");
        Directory.CreateDirectory(Path.GetDirectoryName(eacLog)!);
        File.WriteAllText(eacLog, "[16:16:38] Launcher finished with: 301");
        var old = Path.Combine(appData, "EasyAntiCheat", "product", "older", "anticheatlauncher.log");
        Directory.CreateDirectory(Path.GetDirectoryName(old)!);
        File.WriteAllText(old, "old");
        File.SetLastWriteTimeUtc(old, since.AddHours(-1));
        var crash = Path.Combine(appData, "Arrowhead", "Helldivers2", "crash_data", "report", "params.txt");
        Directory.CreateDirectory(Path.GetDirectoryName(crash)!);
        File.WriteAllText(crash, "x");
        var found = InventorySync.CollectGameLogs(sync, since, appData);
        check(File.ReadAllText(Path.Combine(sync, "eac-deployment.log")).Contains("301", StringComparison.Ordinal) &&
            !File.Exists(Path.Combine(sync, "eac-older.log")), "The run's Easy Anti-Cheat log was not copied, or an older one was.");
        check(found.Count == 2 && found[1].StartsWith("the game wrote ", StringComparison.Ordinal), "A new crash report was not reported: " + string.Join(" | ", found));
    }

    static void Headers(Action<bool, string> check)
    {
        var login = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["User-Agent"] = "Stingray WIN64 : stingray:1 e176252a3ac4eb5436b25e7cef665a8c71afa243",
            ["Accept-Language"] = "de-DE",
            ["Authorization"] = "session 0",
            ["X-Signature"] = new string('a', 44),
            ["Key_ID"] = "1234",
            ["Content-Length"] = "345"
        };
        var now = new DateTimeOffset(2026, 10, 26, 16, 44, 47, TimeSpan.Zero);
        var headers = CaptureHeaders.ForRead(login, now);
        check(headers.Count == 5 && headers["Accept-Language"] == "de-DE" && headers["User-Agent"] == login["User-Agent"] &&
            headers["Accept"] == "application/json" && headers["Content-Type"] == "application/json",
            "The login's language or user agent, or the JSON accept, is missing.");
        check(headers["ct"] == "1793033087000000" && headers["ct"] == CaptureHeaders.Microseconds(now),
            "The client time is not microseconds since the epoch: " + headers["ct"]);
        check(!headers.ContainsKey("Authorization") && !headers.ContainsKey("X-Signature") && !headers.ContainsKey("Key_ID") &&
            !headers.ContainsKey("Content-Length"), "A header of the login request itself was repeated.");

        var bare = CaptureHeaders.ForRead(null, now);
        check(bare["Accept-Language"] == CaptureHeaders.DefaultLanguage && !bare.ContainsKey("User-Agent"),
            "A read without the login's headers has no language, or invented a user agent.");
        var blank = CaptureHeaders.ForRead(new Dictionary<string, string> { ["Accept-Language"] = " " }, now);
        check(blank["Accept-Language"] == CaptureHeaders.DefaultLanguage, "A blank language was repeated.");
        var lower = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["accept-language"] = "ja-JP" };
        check(CaptureHeaders.ForRead(lower, now)["Accept-Language"] == "ja-JP", "A lower-case header name was not found.");
    }

    static void Log(Action<bool, string> check)
    {
        static string Error(string json) => CaptureLog.ErrorText(System.Text.Encoding.UTF8.GetBytes(json));
        check(Error("""{"title":"Bad Request","status":400,"errors":{"version":["The version field is required."]}}""") ==
            "title=Bad Request; status=400; errors.version[0]=The version field is required.", "An error body's fields are not logged.");
        var session = new string('a', 40);
        var redacted = Error($$"""{"error":"denied","session":"{{session}}"}""");
        check(!redacted.Contains(session, StringComparison.Ordinal) && redacted.Contains("<40 characters>", StringComparison.Ordinal), "A token-like value reached the log.");
        check(Error("<html>nope</html>") == "not JSON, 17 bytes" && CaptureLog.ErrorText([]) == "empty body", "A body that is not JSON is logged.");
        check(Error($$"""{"message":"{{new string('x', 20)}} {{new string('y', 400)}}"}""").Length <= 303, "A long reason is not capped.");
        check(Error("{\"message\":\"line\\nbreak\"}") == "message=line break", "A control character reached the log.");
        check(CaptureLog.QueryNames("/api/Progression?season=5&id=123") == "id, season" && CaptureLog.QueryNames("/api/Progression") == "none",
            "Query names are wrong, or values reached the log.");
        check(CaptureLog.Names(["User-Agent", "authorization", "Accept"]) == "Accept, authorization, User-Agent", "Header names are not sorted.");
    }

    static void Backups(string root, Action<bool, string> check)
    {
        var profile = Directory.CreateDirectory(Path.Combine(root, "backups", "player")).FullName;
        var start = new DateTime(2026, 10, 25, 9, 0, 0, DateTimeKind.Utc);
        check(ProfileBackups.Save(profile, "import", start) == null && ProfileBackups.List(profile).Count == 0, "A profile without profile.json was backed up.");
        File.WriteAllText(Path.Combine(profile, "profile.json"), "{\"n\":0}");
        for (var i = 0; i < ProfileBackups.Keep + 3; i++)
        {
            File.WriteAllText(Path.Combine(profile, "profile.json"), "{\"n\":" + i + "}");
            ProfileBackups.Save(profile, i % 2 == 0 ? "import" : "restore", start.AddMinutes(i));
        }
        File.WriteAllText(Path.Combine(profile, ProfileBackups.Folder, "notes.json"), "{}");
        var backups = ProfileBackups.List(profile);
        check(backups.Count == ProfileBackups.Keep, "Old backups were not pruned: " + backups.Count);
        check(backups[0].SavedUtc == start.AddMinutes(ProfileBackups.Keep + 2) && backups[0].Reason == "import" && File.ReadAllText(backups[0].Path) == "{\"n\":12}",
            "The newest backup is not first or not the profile copy.");
        check(backups.Zip(backups.Skip(1)).All(pair => pair.First.SavedUtc > pair.Second.SavedUtc), "Backups are not newest first.");
        check(RefusesArgument(() => ProfileBackups.Save(profile, "../escape", start)), "A backup reason that is not a word was taken.");
    }

    static void Settings(string root, Action<bool, string> check)
    {
        var defaults = new AccountImportSettings();
        check(!defaults.AutoApply && defaults.Includes("level") && defaults.RuleFor("level") == ImportRule.Higher && defaults.RuleFor("armory") == ImportRule.Merge,
            "Wrong defaults for numbers or items.");
        check(!defaults.Includes("ship") && !defaults.Includes("onboarding"), "Ship and tutorial progress are imported by default.");
        check(RefusesArgument(() => defaults.Set("level", true, ImportRule.Merge)), "Level took a rule it does not offer.");
        check(RefusesArgument(() => defaults.Set("super-credits", true, ImportRule.Vanilla)), "An unknown category was taken.");

        var path = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "import-settings")).FullName, AccountImportSettings.FileName);
        var chosen = new AccountImportSettings { AutoApply = true };
        chosen.Set("level", true, ImportRule.Vanilla);
        chosen.Set("armory", false, ImportRule.Merge);
        chosen.Set("ship", true, ImportRule.Vanilla);
        chosen.Save(path);
        var loaded = AccountImportSettings.Load(path);
        check(loaded.AutoApply && loaded.RuleFor("level") == ImportRule.Vanilla && !loaded.Includes("armory") && loaded.Includes("ship"), "Saved choices did not load back.");

        File.WriteAllText(path, """{"autoApply":"yes","categories":{"level":{"include":false,"rule":"Merge"},"armory":{"include":false,"rule":7},"packages":{"include":false,"rule":"Vanilla"}}}""");
        var edited = AccountImportSettings.Load(path);
        check(!edited.AutoApply && edited.Includes("level") && edited.RuleFor("level") == ImportRule.Higher && edited.Includes("armory"),
            "A hand-edited rule a category does not offer was taken.");
        check(!edited.Includes("packages") && edited.RuleFor("packages") == ImportRule.Vanilla, "A valid hand-edited choice was dropped.");
        check(!AccountImportSettings.Load(Path.Combine(root, "import-settings", "missing.json")).AutoApply, "A missing file is not the defaults.");

        // Warbond unlocks have no checkbox: as in vanilla by default, every item, or none; never left out by include alone.
        var warbonds = new AccountImportSettings();
        check(warbonds.Includes("warbondItems") && warbonds.RuleFor("warbondItems") == ImportRule.Merge, "Warbond unlocks are not as in vanilla by default.");
        warbonds.Set("warbondItems", false, ImportRule.All);
        check(warbonds.Includes("warbondItems") && warbonds.RuleFor("warbondItems") == ImportRule.All, "Unchecking a category without a checkbox left it out.");
        warbonds.Set("warbondItems", true, ImportRule.None);
        check(!warbonds.Includes("warbondItems"), "None still imports warbond unlocks.");
        check(RefusesArgument(() => warbonds.Set("warbondItems", true, ImportRule.Vanilla)) && RefusesArgument(() => warbonds.Set("armory", true, ImportRule.All)),
            "A rule a category does not offer was taken.");
        File.WriteAllText(path, """{"categories":{"warbondItems":{"include":false,"rule":"Merge"}}}""");
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.None && !AccountImportSettings.Load(path).Includes("warbondItems"),
            "An older file that left warbond items out does not load as None.");
        File.WriteAllText(path, """{"categories":{"warbondItems":{"include":true,"rule":"Vanilla"}}}""");
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.Merge, "An older Use vanilla for warbond items was not reset to the default.");
        warbonds.Save(path);
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.None, "None did not save and load back.");
    }

    static bool RefusesArgument(Action action)
    {
        try { action(); }
        catch (ArgumentException) { return true; }
        return false;
    }
}
