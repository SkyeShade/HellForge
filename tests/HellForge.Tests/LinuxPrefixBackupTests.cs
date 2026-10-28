using System.Diagnostics;
using HellForge.Core;

// The decisions behind Linux prefix backup and restore: the archive name and compressor, the free-space estimates, the
// listing checks (one top-level folder, no absolute or parent paths, no device files), and a real tar round trip.
static class LinuxPrefixBackupTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux prefix backups name the compressor and default to ~/hellforge-backups", () =>
        {
            check(LinuxPrefixBackup.DefaultFolder("/home/me") == Path.Combine("/home/me", "hellforge-backups"),
                "The default backup folder changed.");
            check(LinuxPrefixBackup.ChooseCompression(true) == LinuxPrefixCompression.Zstd &&
                LinuxPrefixBackup.ChooseCompression(false) == LinuxPrefixCompression.Gzip, "The compressor choice is wrong.");
            check(LinuxPrefixBackup.Extension(LinuxPrefixCompression.Zstd) == ".tar.zst" &&
                LinuxPrefixBackup.Extension(LinuxPrefixCompression.Gzip) == ".tar.gz", "The archive extension is wrong.");
            var now = new DateTime(2026, 10, 27, 21, 30, 5);
            check(LinuxPrefixBackup.ArchiveName(now, LinuxPrefixCompression.Zstd) ==
                "hellforge-prefix-20261027-213005.tar.zst", "The zstd archive name is wrong.");
            check(LinuxPrefixBackup.ArchiveName(now, LinuxPrefixCompression.Gzip) ==
                "hellforge-prefix-20261027-213005.tar.gz", "The gzip archive name is wrong.");
            check(LinuxPrefixBackup.ArchiveName(now, LinuxPrefixCompression.Zstd, 2) ==
                "hellforge-prefix-20261027-213005-2.tar.zst", "The repeated-name suffix is wrong.");
            check(LinuxPrefixBackup.TryCompression("/tmp/a.tar.zst", out var zstd) && zstd == LinuxPrefixCompression.Zstd &&
                LinuxPrefixBackup.TryCompression("/tmp/a.tar.gz", out var gzip) && gzip == LinuxPrefixCompression.Gzip &&
                !LinuxPrefixBackup.TryCompression("/tmp/a.tar", out _), "The archive suffix check is wrong.");
            var folder = Path.Combine(root, "linux-prefix-backup-names");
            Directory.CreateDirectory(folder);
            var first = LinuxPrefixBackup.ArchivePath(folder, now, LinuxPrefixCompression.Zstd);
            File.WriteAllText(first, "");
            check(Path.GetFileName(LinuxPrefixBackup.NextArchive(folder, now, LinuxPrefixCompression.Zstd)) ==
                "hellforge-prefix-20261027-213005-2.tar.zst", "An existing archive was not avoided.");
        });

        test("Linux prefix backup and restore reserve the folder or unpacked size plus the disk margin", () =>
        {
            const long bytes = 5_000_000;
            var backupNeeded = bytes + DiskSpace.Margin;
            check(LinuxPrefixBackup.BackupRequiredBytes(bytes) == backupNeeded, "The backup estimate is wrong.");
            check(LinuxPrefixBackup.BackupSpaceProblem(backupNeeded, bytes) == null, "Enough backup space was refused.");
            check(LinuxPrefixBackup.BackupSpaceProblem(backupNeeded - 1, bytes) != null, "Too little backup space was accepted.");
            var restoreNeeded = bytes + DiskSpace.Margin;
            check(LinuxPrefixBackup.RestoreRequiredBytes(bytes) == restoreNeeded, "The restore estimate is wrong.");
            check(LinuxPrefixBackup.RestoreSpaceProblem(restoreNeeded, bytes) == null, "Enough restore space was refused.");
            check(LinuxPrefixBackup.RestoreSpaceProblem(restoreNeeded - 1, bytes) != null, "Too little restore space was accepted.");
            check(LinuxPrefixBackup.BackupSpaceProblem(null, bytes) == null, "An unreadable file system refused a backup.");
            check(LinuxPrefixBackup.RestoreSpaceProblem(null, bytes) == null, "An unreadable file system refused a restore.");
            check(LinuxPrefixBackup.BackupRequiredBytes(-1) == DiskSpace.Margin, "A negative size was not clamped.");
        });

        test("A prefix archive must hold one top-level folder and no absolute, parent or device entries", () =>
        {
            var good = new[]
            {
                new LinuxArchiveEntry('d', 0, "hellforge-prefix/"),
                new LinuxArchiveEntry('d', 0, "hellforge-prefix/pfx"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/c:", "../drive_c"),
                new LinuxArchiveEntry('-', 123, "hellforge-prefix/pfx/drive_c/file name")
            };
            var accepted = LinuxPrefixBackup.CheckEntries(good);
            check(accepted.Safe && accepted.TopLevel == "hellforge-prefix" && accepted.UncompressedBytes == 123,
                "A normal prefix listing was refused: " + accepted.Reason);
            check(!LinuxPrefixBackup.CheckEntries([]).Safe, "An empty archive was accepted.");
            var bad = new[]
            {
                new LinuxArchiveEntry('-', 0, "/etc/passwd"),
                new LinuxArchiveEntry('-', 0, "hellforge-prefix/../outside"),
                new LinuxArchiveEntry('-', 0, "other/file"),
                new LinuxArchiveEntry('-', 0, "hellforge-prefix"),
                new LinuxArchiveEntry('c', 0, "hellforge-prefix/dev/null"),
                new LinuxArchiveEntry('b', 0, "hellforge-prefix/dev/sda"),
                new LinuxArchiveEntry('h', 0, "hellforge-prefix/hard", "hellforge-prefix/file"),
                new LinuxArchiveEntry('p', 0, "hellforge-prefix/fifo"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/link", "/etc/passwd")
            };
            foreach (var entry in bad)
            {
                var refused = LinuxPrefixBackup.CheckEntries([good[0], entry]);
                check(!refused.Safe, "An unsafe entry was accepted: " + entry.Name);
            }
            var twoTops = LinuxPrefixBackup.CheckEntries([good[0], good[3], new LinuxArchiveEntry('-', 0, "other/file")]);
            check(!twoTops.Safe, "Entries from two top-level folders were accepted.");
        });

        test("The standard Wine dosdevices links pass and their targets are the only outside links allowed", () =>
        {
            var standard = new[]
            {
                new LinuxArchiveEntry('d', 0, "hellforge-prefix/"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/c:", "../drive_c"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/z:", "/"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/com1", "/dev/ttyS0"),
                new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/lpt1", "/dev/lp0")
            };
            var accepted = LinuxPrefixBackup.CheckEntries(standard);
            check(accepted.Safe, "The standard dosdevices links were refused: " + accepted.Reason);
            foreach (var outside in new[]
                     {
                         new LinuxArchiveEntry('l', 0, "hellforge-prefix/link", "/etc/passwd"),
                         new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/com1", "/etc/passwd"),
                         new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/z:", "/home"),
                         new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/c:", "../../../outside")
                     })
            {
                var refused = LinuxPrefixBackup.CheckEntries([standard[0], outside]);
                check(!refused.Safe, "An outside link was accepted: " + outside.Name + " -> " + outside.Target);
            }
        });

        test("An entry under a link entry is refused in either order", () =>
        {
            var top = new LinuxArchiveEntry('d', 0, "hellforge-prefix/");
            var link = new LinuxArchiveEntry('l', 0, "hellforge-prefix/pfx/dosdevices/z:", "/");
            var escape = new LinuxArchiveEntry('-', 0, "hellforge-prefix/pfx/dosdevices/z:/tmp-escape");
            check(!LinuxPrefixBackup.CheckEntries([top, link, escape]).Safe, "An entry after a link was accepted.");
            check(!LinuxPrefixBackup.CheckEntries([top, escape, link]).Safe, "An entry before a link was accepted.");
            check(!LinuxPrefixBackup.CheckEntries([top, link, new LinuxArchiveEntry('-', 0, "hellforge-prefix/pfx/dosdevices/z:")]).Safe,
                "An entry at a link's path was accepted.");
        });

        test("The restore helper includes the shared prefix-in-use function", () =>
        {
            var helper = LinuxPrefixBackup.RestoreHelperScript;
            check(helper.Contains("prefix_in_use()", StringComparison.Ordinal) &&
                helper.Contains("WINEPREFIX=$prefix/pfx", StringComparison.Ordinal) &&
                !helper.Contains(LinuxHelperScripts.PrefixInUseMarker, StringComparison.Ordinal),
                "The restore helper does not include the shared prefix-in-use function.");
        });

        test("The verbose GNU tar listing keeps names, sizes and link targets apart", () =>
        {
            var parsed = LinuxPrefixBackup.ParseListing([
                "drwxr-xr-x me/me 0 2026-10-27 21:30 hellforge-prefix/",
                "-rw-r--r-- me/me 42 2026-10-27 21:30 hellforge-prefix/pfx/drive_c/file name",
                "lrwxrwxrwx me/me 0 2026-10-27 21:30 hellforge-prefix/a -> b",
                "lrwxrwxrwx me/me 0 2026-10-27 21:30 hellforge-prefix/pfx/dosdevices/c: -> ../drive_c",
                "hrw-r--r-- me/me 0 2026-10-27 21:30 hellforge-prefix/hard link to hellforge-prefix/file"
            ]);
            check(parsed.Count == 5 && parsed[0].Kind == 'd' && parsed[1].Kind == '-' && parsed[1].Size == 42 &&
                parsed[1].Name == "hellforge-prefix/pfx/drive_c/file name", "The tar listing was not parsed as expected.");
            check(parsed[2].Kind == 'l' && parsed[2].Name == "hellforge-prefix/a" && parsed[2].Target == "b",
                "A link's name and target were not split.");
            check(parsed[3].Kind == 'l' && parsed[3].Name == "hellforge-prefix/pfx/dosdevices/c:" &&
                parsed[3].Target == "../drive_c", "The dosdevices link was not split.");
            check(parsed[4].Kind == 'h' && parsed[4].Name == "hellforge-prefix/hard" &&
                parsed[4].Target == "hellforge-prefix/file", "A hard link's name and target were not split.");
            check(!LinuxPrefixBackup.CheckEntries([parsed[0], parsed[4]]).Safe, "A hard link was accepted.");
            reject(() => LinuxPrefixBackup.ParseListing(["this is not a tar listing"]));
            reject(() => LinuxPrefixBackup.ParseListing(["lrwxrwxrwx me/me 0 2026-10-27 21:30 hellforge-prefix/a"]));
        });

        test("Linux prefix backup and restore round-trip a small prefix with the real tar", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: native Linux prefix backup)"); return; }
            var sandbox = Path.Combine(root, "linux-prefix-backup-roundtrip");
            var parent = Path.Combine(sandbox, "parent");
            var prefix = Path.Combine(parent, "hellforge-prefix");
            var drive = Path.Combine(prefix, "pfx", "drive_c");
            var documents = Path.Combine(drive, "users", "steamuser", "Documents", "HellForge");
            Directory.CreateDirectory(documents);
            var kept = Path.Combine(documents, "profile.json");
            File.WriteAllText(kept, "first version");
            var dosdevices = Path.Combine(prefix, "pfx", "dosdevices");
            Directory.CreateDirectory(dosdevices);
            var driveLink = Path.Combine(dosdevices, "c:");
            File.CreateSymbolicLink(driveLink, Path.Combine("..", "drive_c"));
            var zLink = Path.Combine(dosdevices, "z:");
            File.CreateSymbolicLink(zLink, "/");
            var comLink = Path.Combine(dosdevices, "com1");
            File.CreateSymbolicLink(comLink, "/dev/ttyS0");

            var backups = Path.Combine(sandbox, "backups");
            Directory.CreateDirectory(backups);
            var compression = LinuxPrefixBackup.ChooseCompression(LinuxPrefixBackup.ZstdInstalled());
            var archive = Path.Combine(backups,
                LinuxPrefixBackup.ArchiveName(new DateTime(2026, 10, 27, 21, 30, 5), compression));
            var result = LinuxPrefixBackup.Backup(prefix, archive, compression, null, CancellationToken.None);
            check(File.Exists(archive) && result.Bytes > 0 && result.Compression == compression,
                "The backup was not written.");
            check(Path.GetFileName(archive).EndsWith(LinuxPrefixBackup.Extension(compression), StringComparison.Ordinal),
                "The archive name does not say which compressor it uses.");
            var listing = LinuxPrefixBackup.ListArchive(archive, compression);
            check(listing.Safe && listing.TopLevel == Path.GetFileName(prefix), "The backup listing is wrong: " + listing.Reason);
            check(listing.UncompressedBytes >= "first version".Length, "The listing's unpacked size is too small.");

            File.WriteAllText(kept, "changed");
            File.WriteAllText(Path.Combine(drive, "added.txt"), "added");
            var plan = LinuxPrefixBackup.PrepareRestore(archive, prefix, "test-token",
                new LinuxUninstallLocations(Path.Combine(sandbox, "home")), null, Path.Combine(documents, "HellForge"));
            check(plan.TopLevel == Path.GetFileName(prefix) && Directory.Exists(plan.StagedFolder),
                "The restore was not staged next to the prefix.");

            var helperScript = Path.Combine(sandbox, "restore-helper.sh");
            var log = Path.Combine(sandbox, "restore.log");
            File.WriteAllText(helperScript, LinuxPrefixBackup.RestoreHelperScript);
            var start = new ProcessStartInfo("bash")
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            foreach (var argument in new[] { helperScript, prefix, plan.StagedFolder, plan.StagingRoot, plan.OldFolder,
                         log, "0", "" })
                start.ArgumentList.Add(argument);
            using var helper = Process.Start(start) ?? throw new IOException("The restore helper did not start.");
            helper.WaitForExit();
            var error = helper.StandardError.ReadToEnd();
            check(helper.ExitCode == 0, "The restore helper failed: " + error);
            check(File.ReadAllText(kept) == "first version", "The restored file is not the backup's.");
            check(!File.Exists(Path.Combine(drive, "added.txt")), "The changed prefix was left in place.");
            check(LinuxUninstall.IsLink(driveLink) && LinuxUninstall.IsLink(zLink) && LinuxUninstall.IsLink(comLink),
                "The restored prefix lost a dosdevices link.");
            check(!Directory.Exists(plan.OldFolder) && !Directory.Exists(plan.StagingRoot),
                "The swap left its old or staging folder behind.");
            check(File.ReadAllText(log).Contains("Restore finished", StringComparison.Ordinal),
                "The restore log has no finish line.");
        });
    }
}
