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

        test("The capture's hosts line never joins the player's last entry, cleanup removes only the capture's lines, and a read-only, missing or briefly held hosts file ends as it was", () => Hosts(root, check));

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

        test("allow-capture.sh writes the sudoers rule that sorts last and removes the rule under its old name", () =>
            HelperInstallerRule(root, check));

        test("On SteamOS allow-capture.sh installs the helper in /opt/hellforge and lists its rule for updates to keep", () =>
            HelperInstallerSteamOs(root, check));

        test("allow-capture.sh installs the helper only in a folder the account cannot change, and a removal runs no other", () =>
            HelperInstallerFolders(root, check));

        test("The launcher finds the helper in the folder this system's installer chose", () => HelperFolders(check));

        test("A sudo that still asks for a password is a reason to install the helper again, and is said after an install", () =>
            HelperCheck(root, check));

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

        test("A capture that does not start says why: its own reason, Windows' refusal, a crash or a kill, and names the security program and file to allow",
            () => StartFailures(root, check));

        test("The capture logs a refused request's short fields, never a token-like value, and only names of headers and query parameters", () =>
            Log(check));

        test("The capture is passive: it keeps only the game's own reads after a login, once each, and completes on the inventory with the progression or after waiting for it", () =>
            Completion(check));

        test("The capture's diagnostics give shapes, causes and a tally without values, and a sync collects the game's anti-cheat and crash logs", () =>
            Diagnostics(root, check));

        test("The capture grants the launcher's Windows account its sync folder, by SID, so a standard account's launcher sees it", () =>
            Grants(root, check));

        test("A sync that ends or stalls without an inventory says whether the game never started, closed, or ran without logging in", () =>
            Outcomes(check));

        test("Steam's 32-bit account id is the SteamID64 of an individual account", () =>
            check(SteamAccount.FromAccountId(39734273) == 76561198000000001, "Wrong SteamID64."));

        test("Profile backups are kept newest first, ten at most, and only profile copies count", () => Backups(root, check));

        test("Import choices default per category, keep what the file sets and refuse rules a category does not offer", () => Settings(root, check));
    }

    static void Grants(string root, Action<bool, string> check)
    {
        const string Launcher = "S-1-5-21-1004336348-1177238915-682003330-1001", Admin = "S-1-5-21-1004336348-1177238915-682003330-500";
        var sync = Path.Combine(root, "sync-grants", "logs", "sync-1");
        var snapshot = Path.Combine(root, "sync-grants", InventorySync.SnapshotFolder, "player");
        var arguments = InventorySync.CaptureProxyArguments(sync, snapshot, Account, Launcher);
        var at = Array.IndexOf(arguments, "--grant-sid");
        check(at >= 0 && at + 1 < arguments.Length && arguments[at + 1] == Launcher, "The launcher's account is not passed to the capture.");
        check(Directory.Exists(sync), "The sync folder is not made before the capture starts.");
        check(!InventorySync.CaptureProxyArguments(sync, snapshot, Account, null).Contains("--grant-sid"), "A grant without an account.");
        check(!InventorySync.CaptureProxyArguments(sync, snapshot, Account, "Everyone").Contains("--grant-sid"), "A name passed as a SID.");
        check(!InventorySync.CaptureProxyArguments(sync, snapshot, Account, Launcher + " *S-1-1-0:F").Contains("--grant-sid"), "A SID with an extra grant passed.");

        check(CaptureAccess.IsSid(Launcher) && CaptureAccess.IsSid("S-1-5-18"), "A real SID refused.");
        foreach (var bad in new[] { "", "S-1", "S-1-5-", "*S-1-5-18", "S-1-5-18:F", "S-1-5-18 /grant", "S-1-5-18\n", "BUILTIN\\Users" })
            check(!CaptureAccess.IsSid(bad), "Not a SID accepted: " + bad);

        var other = CaptureAccess.SyncFolderAclArguments(@"C:\HF\logs\sync-1", Admin, Launcher);
        check(other == $"\"C:\\HF\\logs\\sync-1\" /inheritance:r /grant:r *{Admin}:(OI)(CI)F *{Launcher}:(OI)(CI)F",
            "A capture under another account does not grant the launcher's: " + other);
        var same = CaptureAccess.SyncFolderAclArguments(@"C:\HF\logs\sync-1", Launcher, Launcher);
        check(same == $"\"C:\\HF\\logs\\sync-1\" /inheritance:r /grant:r *{Launcher}:(OI)(CI)F", "The same account granted twice: " + same);
        check(CaptureAccess.SyncFolderAclArguments("x", Admin, null).EndsWith($"*{Admin}:(OI)(CI)F", StringComparison.Ordinal), "No grant without a launcher account.");

        check(InventorySync.CanReadSyncFolder(sync), "An ordinary folder is unreadable.");
        // Only a refusal is another account's folder; a missing one is reported by what reads it.
        check(InventorySync.CanReadSyncFolder(Path.Combine(sync, "missing")), "A missing folder is reported as another account's.");
        check(InventorySync.FinalizeCapture(Path.Combine(sync, "missing"), snapshot) == "The capture finished without an inventory.",
            "A missing capture is not reported as one without an inventory.");
    }

    static void Outcomes(Action<bool, string> check)
    {
        check(InventorySync.GameState(false, false, false) == SyncGameState.NotAsked, "Not asked.");
        check(InventorySync.GameState(true, false, false) == SyncGameState.NotStarted, "Not started.");
        check(InventorySync.GameState(true, true, true) == SyncGameState.Running, "Running.");
        check(InventorySync.GameState(true, true, false) == SyncGameState.Closed, "Closed.");

        check(InventorySync.WaitingProblem(SyncGameState.NotStarted, TimeSpan.FromSeconds(10)) == null, "Steam was not given time to start the game.");
        check(InventorySync.WaitingProblem(SyncGameState.NotStarted, InventorySync.GameStartGrace)?.Contains("Steam has not started", StringComparison.Ordinal) == true,
            "A game Steam never started is not said.");
        check(InventorySync.WaitingProblem(SyncGameState.Closed, TimeSpan.Zero)?.Contains("closed before", StringComparison.Ordinal) == true, "A closed game is not said.");
        check(InventorySync.WaitingProblem(SyncGameState.Running, TimeSpan.FromHours(1)) == null, "A running game is a problem.");

        const string Log = @"C:\HF\logs\sync-1\proxy.log";
        var messages = new[] { SyncGameState.NotAsked, SyncGameState.NotStarted, SyncGameState.Closed, SyncGameState.Running }
            .Select(state => InventorySync.NothingCaptured(state, 10, Log)).ToList();
        check(messages.Distinct().Count() == 4, "Two outcomes read the same.");
        check(messages[0].Contains("could not start", StringComparison.Ordinal) && messages[0].Contains(Log, StringComparison.Ordinal), "A capture that never listened.");
        check(messages[1].Contains("never started", StringComparison.Ordinal), "A game that never started.");
        check(messages[2].Contains("closed before", StringComparison.Ordinal), "A game that closed.");
        check(messages[3].Contains("title screen", StringComparison.Ordinal), "A game that ran without logging in.");
        check(messages.All(message => !message.Contains("administrator", StringComparison.OrdinalIgnoreCase)), "A message tells the player to run as administrator.");
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
            check(lines.Any(line => line.StartsWith(CaptureHosts.Marker, StringComparison.Ordinal)) && lines.Contains("127.0.0.1 " + Host), name + ": the capture's lines are not lines of their own.");
            check(InventorySync.HasLeftoverRedirect(hosts), name + ": the Linux hosts path did not detect the leftover marker.");
            check(!original.Contains("foo", StringComparison.Ordinal) || lines.Contains("0.0.0.0 foo.com"), name + ": the player's last entry was changed.");
            check(CaptureHosts.Remove(hosts, Host), name + ": cleanup found nothing.");
            check(!InventorySync.HasLeftoverRedirect(hosts), name + ": the Linux hosts path still reports a leftover marker.");
            check(File.ReadAllText(hosts) == original.Replace("\r\n", Environment.NewLine, StringComparison.Ordinal), name + ": cleanup did not leave the file as it was, final newline or not.");
            check(!CaptureHosts.Remove(hosts, Host), name + ": a second cleanup removed more.");
        }
        check(!CaptureHosts.Remove(Path.Combine(folder, "missing"), Host), "Cleanup of a missing file removed something.");

        // No hosts file at all (and no etc folder): Add creates one, cleanup deletes it again.
        var missing = Path.Combine(folder, "no-etc", "hosts");
        CaptureHosts.Add(missing, Host);
        check(File.ReadAllLines(missing).Any(line => line.Contains(CaptureHosts.Marker, StringComparison.Ordinal) && line.Contains(CaptureHosts.CreatedFlag, StringComparison.Ordinal)),
            "A created hosts file does not say so.");
        check(CaptureHosts.Remove(missing, Host) && !File.Exists(missing), "Cleanup kept the hosts file the capture created.");

        // The launcher's message for a failed hosts write.
        check(CaptureHosts.ReadFailure(folder) == null, "A failure is read without its file.");
        CaptureHosts.WriteFailure(folder, "Another program kept the hosts file open.");
        check(CaptureHosts.ReadFailure(folder) == "Another program kept the hosts file open.", "The failure did not round-trip.");
        check(new HostsFileException(HostsFailure.Blocked, @"C:\Windows\System32\drivers\etc\hosts", null).Message.Contains("HostsFileHijack", StringComparison.Ordinal),
            "A refused write does not name Defender's detection.");

        // The read-only attribute and file locks are Windows behaviour.
        if (!OperatingSystem.IsWindows()) return;
        foreach (var (name, original) in new[] { ("read-only-no-newline", "# player\r\n0.0.0.0 foo.com"), ("read-only", "0.0.0.0 foo.com\r\n") })
        {
            var hosts = Path.Combine(folder, name);
            File.WriteAllText(hosts, original);
            File.SetAttributes(hosts, FileAttributes.ReadOnly | FileAttributes.Archive);
            CaptureHosts.Add(hosts, Host);
            check(File.GetAttributes(hosts).HasFlag(FileAttributes.ReadOnly), name + ": Add left the hosts file writable.");
            var lines = File.ReadAllLines(hosts);
            check(lines.Contains("127.0.0.1 " + Host) && lines.Contains("0.0.0.0 foo.com"), name + ": Add did not write past read-only, or changed the player's entry.");
            check(lines.Any(line => line.Contains(CaptureHosts.ReadOnlyFlag, StringComparison.Ordinal)), name + ": the marker does not record read-only.");
            // A crash between the write and the attribute: cleanup still restores read-only.
            File.SetAttributes(hosts, FileAttributes.Archive);
            check(CaptureHosts.Remove(hosts, Host), name + ": cleanup found nothing.");
            check(File.GetAttributes(hosts).HasFlag(FileAttributes.ReadOnly), name + ": cleanup after a crash left the hosts file writable.");
            check(File.ReadAllText(hosts) == original.Replace("\r\n", Environment.NewLine, StringComparison.Ordinal), name + ": cleanup changed the player's entries or their final newline.");
            File.SetAttributes(hosts, FileAttributes.Normal);
        }

        var locked = Path.Combine(folder, "locked");
        File.WriteAllText(locked, "0.0.0.0 foo.com\r\n");
        var retry = CaptureHosts.LockedRetry;
        try
        {
            CaptureHosts.LockedRetry = TimeSpan.FromMilliseconds(400);
            using (new FileStream(locked, FileMode.Open, FileAccess.Read, FileShare.None))
            {
                HostsFailure? failure = null;
                try { CaptureHosts.Add(locked, Host); }
                catch (HostsFileException error) { failure = error.Failure; }
                check(failure == HostsFailure.InUse, "A hosts file held open is not reported as in use.");
            }
            // Held open only briefly (an antivirus scan): the retry gets through.
            CaptureHosts.LockedRetry = TimeSpan.FromSeconds(5);
            var holder = new FileStream(locked, FileMode.Open, FileAccess.Read, FileShare.None);
            var release = Task.Delay(500).ContinueWith(_ => holder.Dispose(), TaskScheduler.Default);
            CaptureHosts.Add(locked, Host);
            release.Wait();
            check(File.ReadAllLines(locked).Contains("127.0.0.1 " + Host), "A briefly held hosts file was not retried.");
            check(CaptureHosts.Remove(locked, Host) && File.ReadAllLines(locked).SequenceEqual(["0.0.0.0 foo.com"]), "Cleanup after a retry changed the player's entries.");
        }
        finally { CaptureHosts.LockedRetry = retry; }
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

        var description = LinuxCapture.HelperInstallDescription(install, steamOs: false);
        var onSteamOs = LinuxCapture.HelperInstallDescription(install, steamOs: true);
        check(description.Contains("to /usr/local/lib/hellforge (to /opt/hellforge or /var/lib/hellforge if", StringComparison.Ordinal) &&
            !description.Contains("SteamOS", StringComparison.Ordinal) &&
            onSteamOs.Contains("to /opt/hellforge (to /var/lib/hellforge if", StringComparison.Ordinal) &&
            onSteamOs.Contains(LinuxCapture.SteamOsKeepListPath, StringComparison.Ordinal),
            "The helper confirmation does not name this system's folders, or SteamOS's keep list: " + onSteamOs);
        check(description.Contains("/usr/local/lib/hellforge", StringComparison.Ordinal) &&
            description.Contains("/etc/sudoers.d/zz-hellforge-capture", StringComparison.Ordinal) &&
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

    // The real installer, run as root of a private user and mount namespace over SteamOS's own drop-ins: its rule must be
    // read last, the rule under the old name must go, and --remove must leave neither.
    static void HelperInstallerRule(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: the sudoers rule's name)"); return; }
        if (FakeRootScripts(root, "helper-rule") == null) return;
        var rule = Path.GetFileName(LinuxCapture.SudoersPath);
        var oldRule = Path.GetFileName(LinuxCapture.LegacySudoersPath);
        var body = string.Join("\n",
            "printf '%s\\n' '%wheel ALL=(ALL) ALL' > /etc/sudoers.d/wheel",
            "printf '%s\\n' '%sudo ALL=(ALL) ALL' > /etc/sudoers.d/sudo",
            "printf '%s\\n' 'Defaults !fqdn' > /etc/sudoers.d/no-fqdn",
            "printf '%s\\n' '%wheel ALL=(root) NOPASSWD: /usr/bin/steamos-prepare-oobe-test' > /etc/sudoers.d/wheel-prepare-oobe-test",
            "printf '%s\\n' 'old rule' > /etc/sudoers.d/" + oldRule,
            "PKEXEC_UID=1000 bash " + FakeRootInstaller + " >/dev/null",
            "echo \"order:$(LC_ALL=C ls /etc/sudoers.d | tr '\\n' ' ')\"",
            "echo \"mode:$(stat -c '%a %u %g' /etc/sudoers.d/" + rule + ")\"",
            "echo \"rule:$(cat /etc/sudoers.d/" + rule + ")\"",
            "echo \"keep:$(ls /etc/atomic-update.conf.d 2>/dev/null | tr '\\n' ' ')\"",
            "printf '%s\\n' 'old rule' > /etc/sudoers.d/" + oldRule,
            "bash " + FakeRootInstaller + " --remove >/dev/null",
            "echo \"left:$(LC_ALL=C ls /etc/sudoers.d /usr/local/lib | tr '\\n' ' ')\"");
        if (RunAsFakeRoot(root, "helper-rule", body) is not { } run) return;
        check(run.ExitCode == 0, "The installer failed as root over SteamOS's drop-ins: " + run.Output);
        var order = FakeRootLine(run.Output, "order").Split(' ', StringSplitOptions.RemoveEmptyEntries);
        check(order.Length > 0 && order[^1] == rule, "HellForge's rule is not the last drop-in sudo reads: " + string.Join(' ', order));
        check(!order.Contains(oldRule), "The rule under its old name was left, where SteamOS's own files override it: " + string.Join(' ', order));
        check(!rule.Contains('.') && !rule.EndsWith('~'), "sudo skips a drop-in whose name has a dot or ends in ~: " + rule);
        check(FakeRootLine(run.Output, "mode") == "440 0 0", "The rule is not root-owned with mode 440: " + FakeRootLine(run.Output, "mode"));
        var helper = LinuxCapture.HelperFolders[0] + "/capture-helper";
        check(FakeRootLine(run.Output, "rule") == $"{FakeRootUser} ALL=(root) NOPASSWD: {helper} check, {helper} start, {helper} stop, {helper} cleanup",
            "The rule does not name exactly the helper's four commands for this account: " + FakeRootLine(run.Output, "rule"));
        check(FakeRootLine(run.Output, "keep").Length == 0, "A system that is not SteamOS got SteamOS's keep list.");
        check(FakeRootLine(run.Output, "left") == "/etc/sudoers.d: no-fqdn sudo wheel wheel-prepare-oobe-test /usr/local/lib:",
            "--remove did not remove both rule names and the helper folder, or removed another file: " + FakeRootLine(run.Output, "left"));
    }

    // SteamOS keeps /usr/local on a read-only image: the real installer, as root of a stand-in SteamOS, must put the
    // helper in /opt/hellforge, name that helper in the rule, list the rule for SteamOS updates and leave nothing behind.
    static void HelperInstallerSteamOs(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: the helper's folder on SteamOS)"); return; }
        if (FakeRootScripts(root, "helper-steamos") == null) return;
        var rule = LinuxCapture.SudoersPath;
        var body = string.Join("\n",
            "mount -t tmpfs -o ro,mode=755 tmpfs /usr/local",
            "mkdir -p /var/lib/hellforge && echo stale > /var/lib/hellforge/capture-helper",
            "PKEXEC_UID=1000 bash " + FakeRootInstaller + " >/dev/null",
            "echo \"files:$(LC_ALL=C ls /opt/hellforge | tr '\\n' ' ')\"",
            "echo \"owner:$(stat -c '%a %u' /opt/hellforge /opt/hellforge/capture-helper /opt/hellforge/capture-relay | tr '\\n' ' ')\"",
            "echo \"rule:$(cat " + rule + ")\"",
            "echo \"keep:$(cat " + LinuxCapture.SteamOsKeepListPath + ") $(stat -c '%a %u' " + LinuxCapture.SteamOsKeepListPath + ")\"",
            "echo \"stale:$(ls /var/lib | tr '\\n' ' ')\"",
            "echo \"check:$(/opt/hellforge/capture-helper check 2>&1 && echo ready)\"",
            "echo \"moved:$(cd /tmp && cp /opt/hellforge/capture-helper . && ./capture-helper check 2>&1 | grep -c /usr/local/lib/hellforge/capture-relay)\"",
            "PKEXEC_UID=1000 bash " + FakeRootInstaller + " >/dev/null && echo again:ok",
            "bash " + FakeRootInstaller + " --remove >/dev/null",
            "echo \"left:$(LC_ALL=C ls /opt /var/lib /etc/sudoers.d /etc/atomic-update.conf.d | tr '\\n' ' ')\"");
        if (RunAsFakeRoot(root, "helper-steamos", body, steamOs: true) is not { } run) return;
        check(run.ExitCode == 0, "The installer failed on a read-only /usr/local: " + run.Output);
        string Line(string name) => FakeRootLine(run.Output, name);
        check(Line("files") == "capture-helper capture-helper.sha256 capture-relay capture-relay.sha256",
            "The helper, relay and hashes are not in /opt/hellforge: " + Line("files"));
        check(Line("owner") == "755 0 755 0 755 0", "The folder, helper or relay is not root's alone: " + Line("owner"));
        const string helper = "/opt/hellforge/capture-helper";
        check(Line("rule") == $"{FakeRootUser} ALL=(root) NOPASSWD: {helper} check, {helper} start, {helper} stop, {helper} cleanup",
            "The rule does not name the helper where it was installed: " + Line("rule"));
        check(Line("keep") == rule + " 644 0", "SteamOS's keep list does not hold exactly the rule's path: " + Line("keep"));
        check(Line("stale").Length == 0, "A helper copy in another folder was left: " + Line("stale"));
        check(Line("check") == "ready", "The installed helper does not find the relay beside it: " + Line("check"));
        check(Line("moved") == "1", "A helper outside the allowed folders did not fall back to the first one's relay.");
        check(Line("again") == "ok", "A second install over the first failed.");
        check(Line("left") == "/etc/atomic-update.conf.d: /etc/sudoers.d: /opt: /var/lib:",
            "--remove left something of the helper on SteamOS: " + Line("left"));
    }

    // The rule lets the account run the helper as root, so the helper's folder must be one the account cannot change:
    // the installer passes over such a folder and, with none left, installs nothing at all.
    static void HelperInstallerFolders(string root, Action<bool, string> check)
    {
        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: the helper's folder checks)"); return; }
        if (FakeRootScripts(root, "helper-folders") == null) return;
        var body = string.Join("\n",
            "mount -t tmpfs -o ro,mode=755 tmpfs /usr/local",
            "chmod 777 /opt",
            "PKEXEC_UID=1000 bash " + FakeRootInstaller + " >/dev/null",
            "echo \"first:$(ls /var/lib/hellforge | wc -l) $(grep -c /var/lib/hellforge/capture-helper " + LinuxCapture.SudoersPath + ") $(ls /opt | wc -l)\"",
            "echo \"keep:$(ls /etc/atomic-update.conf.d 2>/dev/null | wc -l)\"",
            "bash " + FakeRootInstaller + " --remove >/dev/null",
            // The account is in the group of id 0 in this stand-in system, and /var/lib becomes writable by that group.
            "chmod 775 /var/lib",
            "if PKEXEC_UID=1000 bash " + FakeRootInstaller + " > /tmp/refused 2>&1; then echo refused:installed; else echo refused:yes; fi",
            "echo \"why:$(tr '\\n' ' ' < /tmp/refused)\"",
            "echo \"left:$(ls /opt /var/lib /etc/sudoers.d | tr '\\n' ' ')\"",
            // A helper planted where every account can write is never run as root by a removal.
            "mkdir /opt/hellforge && printf '#!/bin/sh\\ntouch /tmp/planted-ran\\n' > /opt/hellforge/capture-helper",
            "chmod 777 /opt/hellforge /opt/hellforge/capture-helper",
            "bash " + FakeRootInstaller + " --remove >/dev/null",
            "echo \"planted:$(ls /tmp/planted-ran 2>/dev/null | wc -l)\"");
        if (RunAsFakeRoot(root, "helper-folders", body) is not { } run) return;
        check(run.ExitCode == 0, "The folder checks did not run to the end: " + run.Output);
        string Line(string name) => FakeRootLine(run.Output, name);
        check(Line("first") == "4 1 0", "With /usr/local read-only and /opt open to every account, the helper did not go " +
            "to /var/lib/hellforge alone: " + Line("first"));
        check(Line("keep") == "0", "A system that is not SteamOS got SteamOS's keep list.");
        check(Line("refused") == "yes", "The installer installed a helper although no folder is root's alone.");
        var why = Line("why");
        check(why.Contains("/usr/local/lib/hellforge: ", StringComparison.Ordinal) &&
            why.Contains("/opt/hellforge: /opt is writable by every account", StringComparison.Ordinal) &&
            why.Contains("/var/lib/hellforge: /var/lib is writable by a group " + FakeRootUser + " is in", StringComparison.Ordinal),
            "The refusal does not say why each folder cannot be used: " + why);
        check(Line("left") == "/etc/sudoers.d: /opt: /var/lib:", "A refused install left a folder or a rule: " + Line("left"));
        check(Line("planted") == "0", "A removal ran a helper that another account could have written.");
    }

    // Checked on every system, also where the installer cannot be run as root of a namespace: the launcher, the installer
    // and the helper name the same folders in the same order, and the launcher looks where the installer installs.
    static void HelperFolders(Action<bool, string> check)
    {
        var folders = string.Join(' ', LinuxCapture.HelperFolders);
        var installer = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperInstaller));
        var helper = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperScript));
        if (File.Exists(installer) && File.Exists(helper))
        {
            var lines = File.ReadAllLines(installer);
            check(lines.Contains("all_folders=(" + folders + ")") && lines.Contains("keep_list=" + LinuxCapture.SteamOsKeepListPath) &&
                lines.Contains("folders=(\"${all_folders[@]}\")") && lines.Contains("  folders=(\"${all_folders[@]:1}\")"),
                "allow-capture.sh and the launcher do not name the same helper folders, order or keep list.");
            check(File.ReadAllLines(helper).Contains("helper_folders=(" + folders + ")"),
                "capture-helper.sh and the launcher do not name the same helper folders.");
        }
        else Console.WriteLine("  (the scripts are not in the test working directory: only the launcher's side is checked)");

        check(LinuxCapture.InstallFolders(steamOs: false).SequenceEqual(LinuxCapture.HelperFolders) &&
            LinuxCapture.InstallFolders(steamOs: true).SequenceEqual(["/opt/hellforge", "/var/lib/hellforge"]),
            "The launcher's install order is not /usr/local first, and /opt first on SteamOS without /usr/local.");
        string Folder(bool steamOs, params string[] present) =>
            LinuxCapture.HelperFolderOf(steamOs, path => present.Contains(path));
        check(Folder(false) == "/usr/local/lib/hellforge" && Folder(true) == "/opt/hellforge",
            "With no helper installed, the folder named is not the system's first choice.");
        check(Folder(false, "/opt/hellforge/capture-helper") == "/opt/hellforge" &&
            Folder(true, "/var/lib/hellforge/capture-helper") == "/var/lib/hellforge",
            "A helper in a later folder was not found.");
        check(Folder(true, "/usr/local/lib/hellforge/capture-helper", "/opt/hellforge/capture-helper") == "/opt/hellforge" &&
            Folder(false, "/usr/local/lib/hellforge/capture-helper", "/opt/hellforge/capture-helper") == "/usr/local/lib/hellforge" &&
            Folder(true, "/usr/local/lib/hellforge/capture-helper") == "/usr/local/lib/hellforge",
            "With two copies, the launcher did not take the one the installer writes first on this system.");
        string[] parts = ["capture-helper", "capture-helper.sha256", "capture-relay", "capture-relay.sha256"];
        check(LinuxCapture.HelperFolders.All(folder => parts.All(part => LinuxCapture.HelperInstalled(path => path == folder + "/" + part))) &&
            LinuxCapture.HelperInstalled(path => path == LinuxCapture.SteamOsKeepListPath) &&
            LinuxCapture.HelperInstalled(path => path == LinuxCapture.SudoersPath) &&
            !LinuxCapture.HelperInstalled(path => path == "/opt/hellforge/unshare"),
            "A leftover of the helper in one of its folders, or SteamOS's keep list, is not counted as installed.");
    }

    // HelperCheckProblem reads sudo's own answer; InstallHelper reports a rule that still does not win without sending the
    // player to a terminal command that would write the same files again.
    static void HelperCheck(string root, Action<bool, string> check)
    {
        check(LinuxCapture.HelperCheckProblem(new LinuxCapture.WineResult(0, "", "")) == null, "A passing helper check was a problem.");
        var password = LinuxCapture.HelperCheckProblem(new LinuxCapture.WineResult(1, "", "sudo: a password is required"));
        check(password != null && password.Contains(LinuxCapture.SudoersPath, StringComparison.Ordinal) &&
            password.Contains("asks for a password", StringComparison.Ordinal),
            "sudo asking for a password was not named with the rule's path: " + password);
        var other = LinuxCapture.HelperCheckProblem(new LinuxCapture.WineResult(1, "", "hellforge-capture-helper: /etc/hosts is not writable"));
        check(other != null && other.Contains("/etc/hosts is not writable", StringComparison.Ordinal) &&
            !other.Contains("asks for a password", StringComparison.Ordinal),
            "The helper's own refusal was not passed on: " + other);

        check(LinuxCapture.InstallFixesCheck(password!) &&
            LinuxCapture.InstallFixesCheck(LinuxCapture.HelperCheckProblem(new LinuxCapture.WineResult(1, "",
                "hellforge-capture-helper: the relay is missing (/x): run allow-capture.sh again."))!) &&
            !LinuxCapture.InstallFixesCheck(other!),
            "The install is not offered for exactly the failures it can clear.");

        // Checked on every system, also where the installer cannot be run as root of a namespace: the launcher and the
        // script name the same two rules, and the new name is read after every drop-in SteamOS ships.
        var script = Path.GetFullPath(Path.Combine("linux", LinuxCapture.HelperInstaller));
        if (File.Exists(script))
        {
            var lines = File.ReadAllLines(script);
            check(lines.Contains("sudoers=" + LinuxCapture.SudoersPath) && lines.Contains("old_sudoers=" + LinuxCapture.LegacySudoersPath),
                "allow-capture.sh and the launcher do not name the same sudoers rules.");
        }
        string[] shipped = ["holo-group-sudo", "holo-group-wheel", "holo-no-fqdn", "no-fqdn", "sudo", "wheel", "wheel-prepare-oobe-test"];
        var name = Path.GetFileName(LinuxCapture.SudoersPath);
        check(shipped.All(file => string.CompareOrdinal(file, name) < 0) &&
            shipped.Any(file => string.CompareOrdinal(file, Path.GetFileName(LinuxCapture.LegacySudoersPath)) > 0),
            "The rule's name does not sort after SteamOS's drop-ins in sudo's byte order, or the old name did.");

        if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: pkexec helper install)"); return; }
        var install = Directory.CreateDirectory(Path.Combine(root, "helper-check", "install")).FullName;
        var installer = Path.Combine(Directory.CreateDirectory(Path.Combine(install, "app", "linux")).FullName, LinuxCapture.HelperInstaller);
        File.WriteAllText(installer, "#!/bin/sh\n");
        var pkexec = FakePkexec(Path.Combine(root, "helper-check", "pkexec"), "exit 0");
        var blocked = LinuxCapture.InstallHelper(install, pkexec, "/usr/bin/bash", _ => null, () => password);
        check(!blocked.Succeeded && !blocked.Cancelled && !blocked.NeedsManual &&
            blocked.Message.Contains("asks for a password", StringComparison.Ordinal) &&
            !blocked.Message.Contains("Install it from a terminal", StringComparison.Ordinal),
            "A rule that does not win after the install was not reported as such: " + blocked.Message);
        var ready = LinuxCapture.InstallHelper(install, pkexec, "/usr/bin/bash", _ => null, () => null);
        check(ready.Succeeded, "An install whose files and rule are right did not succeed: " + ready.Message);
        var stale = LinuxCapture.InstallHelper(install, pkexec, "/usr/bin/bash", _ => "the copies differ", () => throw new Exception("checked"));
        check(!stale.Succeeded && stale.NeedsManual, "sudo was asked although the installed files were already wrong.");
    }

    const string FakeRootUser = "deck";
    const string FakeRootInstaller = "/scripts/" + LinuxCapture.HelperInstaller;

    static string FakeRootLine(string output, string name) => string.Join(' ', (output.Split('\n')
        .FirstOrDefault(line => line.StartsWith(name + ":", StringComparison.Ordinal))?[(name.Length + 1)..] ?? "")
        .Split(' ', StringSplitOptions.RemoveEmptyEntries));

    // A copy of the installer and helper with a stand-in relay beside them, as an installation's app/linux has them; the
    // fake root has it as /scripts. Null (and a skip line) when the scripts are not in the test working directory.
    static string? FakeRootScripts(string root, string name)
    {
        var source = Path.GetFullPath("linux");
        if (!File.Exists(Path.Combine(source, LinuxCapture.HelperInstaller)))
        {
            Console.WriteLine("  (skipped: linux/allow-capture.sh is not in the test working directory)");
            return null;
        }
        var scripts = Directory.CreateDirectory(Path.Combine(root, name, "linux")).FullName;
        foreach (var file in new[] { LinuxCapture.HelperInstaller, LinuxCapture.HelperScript })
            File.Copy(Path.Combine(source, file), Path.Combine(scripts, file), overwrite: true);
        File.WriteAllText(Path.Combine(scripts, LinuxCapture.RelayBinary), "stand-in relay\n");
        return scripts;
    }

    // Runs a bash script as root of a stand-in system: a private user and mount namespace, changed into an empty root
    // that has only this PC's programs (read-only), its own accounts (deck, 1000, also in the group of id 0) and empty
    // /etc, /opt, /var/lib and /usr/local. In a user namespace the PC's own folders belong to nobody, so the installer's
    // "root's alone" checks need folders made inside it; the PC's own files are neither read nor changed. Null (and a
    // skip line) where unprivileged user namespaces cannot mount, as on Ubuntu's runners with AppArmor's restriction.
    static (int ExitCode, string Output)? RunAsFakeRoot(string root, string name, string body, bool steamOs = false)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, name)).FullName;
        var scripts = Directory.CreateDirectory(Path.Combine(folder, "linux")).FullName;
        var system = Directory.CreateDirectory(Path.Combine(folder, "system")).FullName;
        File.WriteAllText(Path.Combine(scripts, "body.sh"), "set -eu\n" + body + "\n");
        var script = Path.Combine(folder, "fake-root.sh");
        File.WriteAllText(script, string.Join("\n",
            "set -eu",
            "PATH=$PATH:/usr/sbin:/sbin",
            "R=" + ShQuote(system),
            "mount -t tmpfs -o mode=755 tmpfs \"$R\"",
            "mkdir -p \"$R\"/usr/local/lib \"$R\"/etc/sudoers.d \"$R\"/run \"$R\"/tmp \"$R\"/proc \"$R\"/dev \"$R\"/opt \"$R\"/var/lib \"$R\"/scripts",
            "for d in /usr/bin /usr/sbin /usr/lib /usr/lib32 /usr/lib64 /usr/libexec /usr/share /bin /sbin /lib /lib32 /lib64; do",
            "  if [ -L \"$d\" ]; then cp -P \"$d\" \"$R$d\"; elif [ -d \"$d\" ]; then mkdir \"$R$d\"; mount --rbind \"$d\" \"$R$d\"; fi",
            "done",
            "mount --rbind /proc \"$R/proc\"",
            "mount --rbind /dev \"$R/dev\"",
            "mount --bind " + ShQuote(scripts) + " \"$R/scripts\"",
            "[ ! -d /etc/alternatives ] || cp -RP /etc/alternatives \"$R/etc/\"",
            "[ ! -f /etc/ld.so.cache ] || cp /etc/ld.so.cache \"$R/etc/\"",
            "printf '%s\\n' 'root:x:0:0::/root:/bin/bash' 'nobody:x:65534:65534::/:/bin/false' '" + FakeRootUser +
                ":x:1000:1000::/home/" + FakeRootUser + ":/bin/bash' > \"$R/etc/passwd\"",
            "printf '%s\\n' 'root:x:0:" + FakeRootUser + "' 'nobody:x:65534:' 'wheel:x:998:" + FakeRootUser + "' '" +
                FakeRootUser + ":x:1000:' > \"$R/etc/group\"",
            "printf '%s\\n' '127.0.0.1 localhost' > \"$R/etc/hosts\"",
            "printf '%s\\n' 'NAME=\"Stand-in\"' 'ID=" + (steamOs ? "steamos" : "standin") + "' > \"$R/etc/os-release\"",
            "exec env -i PATH=/usr/bin:/usr/sbin:/bin:/sbin chroot \"$R\" /bin/bash /scripts/body.sh") + "\n");
        string[] unshare = ["--user", "--map-root-user", "--mount", "bash"];
        try
        {
            var probe = RunTestProcess("unshare", [.. unshare, "-c", "mount -t tmpfs tmpfs " + ShQuote(system)]);
            if (probe.ExitCode != 0)
            {
                Console.WriteLine("  (skipped: this system does not let a user namespace mount a file system)");
                return null;
            }
        }
        catch (System.ComponentModel.Win32Exception)
        {
            Console.WriteLine("  (skipped: unshare is not installed)");
            return null;
        }
        return RunTestProcess("unshare", [.. unshare, script]);
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

    static void StartFailures(string root, Action<bool, string> check)
    {
        const string Proxy = @"C:\HellForge\app\capture\HellForge.CaptureProxy.exe";
        const string Log = @"C:\HellForge\logs\sync-1\proxy.log";

        // Every capture-side problem has an exit code of its own that reads back, clear of 0, 1 and 2.
        foreach (var problem in new[] { CaptureProblem.EvidenceFolder, CaptureProblem.LeftoverCapture, CaptureProblem.HostsBusy,
            CaptureProblem.HostsDenied, CaptureProblem.Offline, CaptureProblem.Certificate, CaptureProblem.PortInUse, CaptureProblem.PortDenied })
        {
            var code = CaptureStart.ExitCode(problem);
            check(code >= 20 && CaptureStart.FromExitCode(code) == problem, problem + ": its exit code does not read back.");
        }
        check(CaptureStart.FromExitCode(0) == CaptureProblem.None && CaptureStart.FromExitCode(1) == CaptureProblem.None &&
            CaptureStart.FromExitCode(2) == CaptureProblem.None, "A finished capture's exit code reads as a start failure.");

        // The failure file wins over the exit code, and a broken or unknown one is ignored.
        var sync = Directory.CreateDirectory(Path.Combine(root, "sync-start")).FullName;
        check(CaptureStart.Read(sync) == null, "A missing failure file reads as a failure.");
        CaptureStart.Write(sync, CaptureProblem.PortInUse, "used by vmware-hostd.exe (process 4242)");
        check(CaptureStart.Read(sync) is { Problem: CaptureProblem.PortInUse, Detail: "used by vmware-hostd.exe (process 4242)" }, "The failure file does not read back.");
        check(CaptureStart.FromExit(CaptureStart.ExitCode(CaptureProblem.Offline), CaptureStart.Read(sync), "") is { Problem: CaptureProblem.PortInUse },
            "The exit code won over the capture's own reason.");
        File.WriteAllText(Path.Combine(sync, CaptureStart.FailureFile), """{"problem":"Teapot"}""");
        check(CaptureStart.Read(sync) == null, "An unknown problem was read.");
        File.WriteAllText(Path.Combine(sync, CaptureStart.FailureFile), "{\"problem\":");
        check(CaptureStart.Read(sync) == null, "A half-written failure file was read.");

        // A hosts failure's one message is the hosts write's own reason (hosts-failure.txt), shown as written.
        const string hostsReason = @"avp.exe (pid 77) kept the hosts file (C:\Windows\System32\drivers\etc\hosts) open, so the sync could not change it. Close it, then sync again.";
        var hosts = CaptureStart.FromExit(CaptureStart.ExitCode(CaptureProblem.HostsBusy), null, null, hostsReason);
        check(hosts is { Problem: CaptureProblem.HostsBusy, Detail: hostsReason } &&
            CaptureStart.FromExit(CaptureStart.ExitCode(CaptureProblem.HostsDenied), null, null, "Windows refused.") is { Problem: CaptureProblem.HostsDenied },
            "A hosts write's reason is not the hosts failure.");
        var hostsMessage = CaptureStart.Message(hosts, ["Kaspersky Premium"], Proxy, Log);
        check(hostsMessage.StartsWith(hostsReason, StringComparison.Ordinal) && hostsMessage.Split("kept the hosts file").Length == 2 &&
            hostsMessage.Contains("Kaspersky Premium may have blocked it", StringComparison.Ordinal), "A hosts failure is not told once, with the program to allow: " + hostsMessage);

        // How a capture ended without a reason of its own. The 2026-10-30 report (#160): the hosts file held by another
        // process crashed the capture before this classification existed.
        const string crashedLog = "01:55:00.625 CA installed\r\n01:55:00.712 crashed: IOException: The process cannot access the file " +
            @"'C:\windows\system32\drivers\etc\hosts' because it is being used by another process." + "\r\n";
        check(CaptureStart.FromExit(CaptureStart.UnhandledException, null, crashedLog) is { Problem: CaptureProblem.Crashed } crash &&
            crash.Detail!.StartsWith("IOException: The process cannot access the file", StringComparison.Ordinal), "A crash is not read from proxy.log.");
        check(CaptureStart.FromExit(CaptureStart.UnhandledException, null, null) is { Problem: CaptureProblem.Crashed, Detail: null }, "A crash without a log is not a crash.");
        check(CaptureStart.FromExit(1, null, null) is { Problem: CaptureProblem.Stopped } silent && silent.Detail!.Contains("wrote nothing", StringComparison.Ordinal),
            "A capture killed before it wrote anything does not say so.");
        check(CaptureStart.FromExit(1, null, "01:55:00.527 upstream 1.2.3.4:443\r\n") is { Problem: CaptureProblem.Stopped } killed &&
            killed.Detail == "exit code 0x00000001", "A capture stopped from outside is not reported as stopped.");
        check(CaptureStart.FromExit(CaptureStart.ExitCode(CaptureProblem.Certificate), null, null) is { Problem: CaptureProblem.Certificate, Detail: null },
            "A capture's exit code without its failure file is not used.");

        // Windows' answers to the elevated start.
        check(CaptureStart.FromStartError(1223) == CaptureProblem.Declined && CaptureStart.FromStartError(2) == CaptureProblem.Missing &&
            CaptureStart.FromStartError(225) == CaptureProblem.Blocked && CaptureStart.FromStartError(5) == CaptureProblem.Blocked &&
            CaptureStart.FromStartError(4551) == CaptureProblem.Blocked, "Windows' refusal to start the capture is misread.");

        // Security Center's list without Microsoft's own, and the advice names the program and the exact file.
        check(SecurityProducts.ThirdParty(["Windows Defender\r", "Kaspersky Premium", "", "Microsoft Defender Antivirus", "kaspersky premium", "Avast Antivirus"])
            .SequenceEqual(["Kaspersky Premium", "Avast Antivirus"]), "Security Center's names are filtered wrong.");
        var kaspersky = CaptureStart.Message(new CaptureFailure(CaptureProblem.Stopped, "exit code 0x00000001"), ["Kaspersky Premium"], Proxy, Log);
        check(kaspersky.Contains("Kaspersky Premium may have blocked it", StringComparison.Ordinal) && kaspersky.Contains(Proxy, StringComparison.Ordinal) &&
            kaspersky.Contains("Do not restrict application activity", StringComparison.Ordinal) && kaspersky.Contains(Log, StringComparison.Ordinal),
            "A Kaspersky block does not name Kaspersky, the file and its setting: " + kaspersky);
        var two = CaptureStart.Message(new CaptureFailure(CaptureProblem.HostsBusy, "avp.exe (pid 77) kept the hosts file open"), ["Avast Antivirus", "Norton 360"], Proxy, Log);
        check(two.Contains("Avast Antivirus or Norton 360 may have blocked it", StringComparison.Ordinal) && two.Contains("avp.exe (pid 77)", StringComparison.Ordinal) &&
            !two.Contains("Kaspersky", StringComparison.Ordinal), "Two security programs or the hosts file's holder are not named: " + two);
        var none = CaptureStart.Message(new CaptureFailure(CaptureProblem.Blocked, "Operation did not complete successfully because the file contains a virus (Windows error 225)"), [], Proxy, Log);
        check(none.Contains("Windows Security", StringComparison.Ordinal) && none.Contains(Proxy, StringComparison.Ordinal), "A block without a third-party program does not say where to allow it: " + none);

        // What a security program does not cause is not blamed on one, and the player is told what to do instead.
        var port = CaptureStart.Message(new CaptureFailure(CaptureProblem.PortInUse, "used by vmware-hostd.exe (process 4242)"), ["Kaspersky Premium"], Proxy, Log);
        check(port.Contains("vmware-hostd.exe", StringComparison.Ordinal) && port.Contains("close it", StringComparison.Ordinal) &&
            !port.Contains("Kaspersky", StringComparison.Ordinal), "A port held by another program is blamed on the security program: " + port);
        var declined = CaptureStart.Message(new CaptureFailure(CaptureProblem.Declined, null), [], Proxy, Log);
        check(declined.Contains("administrator prompt", StringComparison.Ordinal) && !declined.Contains("security", StringComparison.OrdinalIgnoreCase),
            "A declined prompt is reported as something else: " + declined);
        var offline = CaptureStart.Message(new CaptureFailure(CaptureProblem.Offline, null), [], Proxy, Log);
        check(offline.Contains("internet", StringComparison.Ordinal) && !offline.Contains(Proxy, StringComparison.Ordinal), "No connection is blamed on a security program.");
        foreach (var problem in Enum.GetValues<CaptureProblem>().Where(problem => problem != CaptureProblem.None))
            check(!CaptureStart.Message(new CaptureFailure(problem, null), [], Proxy, Log).Contains("()", StringComparison.Ordinal), problem + ": an empty detail shows.");
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

    static void Completion(Action<bool, string> check)
    {
        const CaptureCompletion.Kind Login = CaptureCompletion.Kind.Login, Progression = CaptureCompletion.Kind.Progression,
            Inventory = CaptureCompletion.Kind.Inventory, Other = CaptureCompletion.Kind.Other;
        check(CaptureCompletion.Classify("POST", "/api/Account/Login") == Login && CaptureCompletion.Classify("GET", "/api/Account/Login") == Other,
            "The login is not the POST to /api/Account/Login.");
        check(CaptureCompletion.Classify("GET", "/api/Progression") == Progression && CaptureCompletion.Classify("GET", "/API/progression") == Progression &&
            CaptureCompletion.Classify("GET", "/api/Progression/inventory") == Inventory, "The game's two reads are not recognised.");
        check(CaptureCompletion.Classify("PUT", "/api/Progression") == Other && CaptureCompletion.Classify("GET", "/api/Progression/Items/Customize/v2") == Other &&
            CaptureCompletion.Classify("PUT", "/api/Progression/ShipName") == Other, "A write or another read was taken for one the capture keeps.");

        var t0 = new DateTime(2026, 11, 4, 21, 25, 11, DateTimeKind.Utc);
        // The order both Linux syncs show: login, progression, inventory.
        var usual = new CaptureCompletion(progressionWanted: true);
        check(usual.Refusal(Progression, true) == "it came before a login" && usual.Refusal(Inventory, true) == "it came before a login",
            "A read before the login was kept.");
        usual.Login();
        check(usual.Refusal(Progression, false) == "the API refused it", "A refused read was kept.");
        check(usual.Refusal(Other, true) != null, "Another request was kept.");
        check(usual.Refusal(Progression, true) == null, "The game's progression after the login was not kept.");
        usual.Saved(Progression, t0);
        check(usual.Refusal(Progression, true) == "it was already kept", "A second progression would overwrite the first.");
        check(!usual.TryComplete(t0), "The capture completed without the inventory.");
        check(usual.Refusal(Inventory, true) == null, "The game's inventory was not kept.");
        usual.Saved(Inventory, t0.AddSeconds(0.2));
        check(usual.TryComplete(t0.AddSeconds(0.2)) && usual.Completed, "Progression then inventory did not complete at once.");
        check(!usual.TryComplete(t0.AddSeconds(1)), "The capture completed twice.");
        check(usual.Refusal(Inventory, true) == "the capture is already complete", "A read after completion was kept.");

        // The inventory first: the progression is waited for, then the capture completes without it.
        var late = new CaptureCompletion(progressionWanted: true);
        late.Login();
        late.Saved(Inventory, t0);
        check(!late.TryComplete(t0) && !late.TryComplete(t0 + CaptureCompletion.ResponseWait - TimeSpan.FromMilliseconds(1)),
            "The capture did not wait for the progression.");
        check(late.TryComplete(t0 + CaptureCompletion.ResponseWait) && !late.HasProgression, "The capture never completed without the progression.");

        var arrives = new CaptureCompletion(progressionWanted: true);
        arrives.Login();
        arrives.Saved(Inventory, t0);
        arrives.Saved(Progression, t0.AddSeconds(2));
        check(arrives.TryComplete(t0.AddSeconds(2)) && arrives.HasProgression, "A progression within the wait did not complete the capture.");

        // No progression asked for (no --progression): the inventory alone completes it, and a progression is not kept.
        var inventoryOnly = new CaptureCompletion(progressionWanted: false);
        inventoryOnly.Login();
        check(inventoryOnly.Refusal(Progression, true) == "the progression was not asked for", "An unasked progression was kept.");
        inventoryOnly.Saved(Inventory, t0);
        check(inventoryOnly.TryComplete(t0), "The inventory alone did not complete a capture without progression.");

        // A refused login opens nothing.
        var refused = new CaptureCompletion(progressionWanted: true);
        check(!refused.LoggedIn && refused.Refusal(Inventory, true) == "it came before a login", "A capture without a login kept a read.");

        // The catalogues the game reads at login: each once, and the warbond list, which comes after the inventory, waited for.
        const CaptureCompletion.Kind Catalogue = CaptureCompletion.Kind.Catalogue;
        check(CaptureCompletion.Classify("GET", "/api/Progression/items") == Catalogue && CaptureCompletion.Classify("GET", "/api/SeasonPass") == Catalogue &&
            CaptureCompletion.Classify("GET", "/api/Progression/levelspec") == Catalogue && CaptureCompletion.Classify("GET", "/api/SeasonPass/1929468580") == Other &&
            CaptureCompletion.Classify("GET", "/api/StoreFront") == Other, "The catalogue reads are not exactly the five the game makes at login.");
        check(CaptureCompletion.CatalogueFile("/API/progression/ITEMS") == "items.json", "A catalogue's file does not follow its path.");
        var withCatalogues = new CaptureCompletion(progressionWanted: true, catalogueWanted: true);
        withCatalogues.Login();
        foreach (var (path, _) in CaptureCompletion.Catalogues.Where(entry => entry.File != "SeasonPass.json"))
        {
            check(withCatalogues.Refusal(Catalogue, true, path) == null, path + " was not kept.");
            withCatalogues.Saved(Catalogue, t0, path);
        }
        check(withCatalogues.Refusal(Catalogue, true, "/api/Progression/items") == "it was already kept", "A catalogue was kept twice.");
        withCatalogues.Saved(Progression, t0);
        withCatalogues.Saved(Inventory, t0);
        check(!withCatalogues.TryComplete(t0.AddSeconds(1)) && withCatalogues.Missing().SequenceEqual(["SeasonPass.json"]),
            "The capture completed before the warbond list, or says something else is missing.");
        withCatalogues.Saved(Catalogue, t0.AddSeconds(1), "/api/SeasonPass");
        check(withCatalogues.HasCatalogues && withCatalogues.TryComplete(t0.AddSeconds(1)), "The warbond list did not complete the capture.");
        var noList = new CaptureCompletion(progressionWanted: true, catalogueWanted: true);
        noList.Login();
        noList.Saved(Progression, t0);
        noList.Saved(Inventory, t0);
        check(noList.TryComplete(t0 + CaptureCompletion.ResponseWait) && !noList.HasCatalogues, "Missing catalogues kept the capture open past the wait.");
        check(new CaptureCompletion(progressionWanted: true).Refusal(Catalogue, true, "/api/SeasonPass") is "it came before a login",
            "A catalogue was kept before a login.");
        var unasked = new CaptureCompletion(progressionWanted: true);
        unasked.Login();
        check(unasked.Refusal(Catalogue, true, "/api/SeasonPass") == "the catalogues were not asked for", "A catalogue was kept without --catalogue.");
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
        // A live-like profile takes everything by default, at each category's first rule; a saved file still wins.
        var liveLike = new AccountImportSettings(ProfileStore.LiveLikeTemplate);
        check(AccountImportSettings.Categories.All(c => liveLike.Includes(c.Id) && liveLike.RuleFor(c.Id) == c.Rules[0]) && !liveLike.AutoApply,
            "A live-like profile does not import every category by default.");
        check(!new AccountImportSettings("starter").Includes("ship"), "A starter profile imports its ship by default.");
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
        check(AccountImportSettings.Load(Path.Combine(root, "import-settings", "missing.json"), ProfileStore.LiveLikeTemplate).Includes("ship") &&
            !AccountImportSettings.Load(path, ProfileStore.LiveLikeTemplate).Includes("packages"), "A live-like profile's defaults or saved file were not used.");

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
