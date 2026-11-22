using System.IO.Compression;
using System.Security.Cryptography;
using HellForge.Core;

// The installer an installation keeps of itself: kept on install, replaced on update (one copy), the refusals (no room,
// a damaged kept file, a missing hash, unsafe names and paths) and the Linux installer folder a copy rebuilds.
static class KeptInstallerTests
{
    static InstallLayout NewInstall(string root, string name)
    {
        var install = new InstallLayout(Path.Combine(root, "kept-installer", name, "HellForge"));
        install.Create("1");
        return install;
    }

    static string Fake(string folder, string name, int seed, int length = 200_000)
    {
        Directory.CreateDirectory(folder);
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        var path = Path.Combine(folder, name);
        File.WriteAllBytes(path, bytes);
        return path;
    }

    static string Sha(string file) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)));

    static DiskSpaceInfo Free(long bytes) => new("X:\\", bytes);

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Setup keeps one copy of itself in the installation and an update replaces it", () =>
        {
            var install = NewInstall(root, "update");
            var downloads = Path.Combine(root, "kept-installer", "update", "Downloads");
            var first = KeptInstaller.Keep(install, Fake(downloads, "HellForge-Setup.exe", 1), InstallerKind.WindowsSetup, "0.4.0-beta (abc1234)");
            check(Path.GetFileName(first.Path) == "HellForge-Setup-0.4.0-beta.exe", "The kept name is not HellForge-Setup-<version>.exe: " + first.Path);
            check(first.Sha256 == Sha(Path.Combine(downloads, "HellForge-Setup.exe")), "The recorded hash is not the installer's.");
            check(File.ReadAllText(first.Path + KeptInstaller.HashSuffix).StartsWith(first.Sha256!.ToLowerInvariant() + " *HellForge-Setup-0.4.0-beta.exe", StringComparison.Ordinal),
                "The .sha256 file is not in sha256sum's format.");
            KeptInstaller.RecordNotKept(install, "an earlier failure");
            var second = KeptInstaller.Keep(install, Fake(downloads, "HellForge-Setup (1).exe", 2), InstallerKind.WindowsSetup, "0.5.0-beta (def5678)");
            var files = Directory.GetFiles(KeptInstaller.Folder(install)).Select(Path.GetFileName).Order(StringComparer.Ordinal).ToArray();
            check(files.SequenceEqual(["HellForge-Setup-0.5.0-beta.exe", "HellForge-Setup-0.5.0-beta.exe.sha256"]),
                "An update did not leave exactly the new installer and its hash: " + string.Join(", ", files));
            var state = KeptInstaller.Find(install);
            check(state.Kept?.Version == "0.5.0-beta" && state.Kept.Sha256 == second.Sha256 && state.NotKept == null,
                "Find did not report the new installer, or kept the earlier failure.");
            // Setup started from the kept copy itself: hashed again, not copied over itself
            var again = KeptInstaller.Keep(install, second.Path, InstallerKind.WindowsSetup, "0.5.0-beta");
            check(again.Sha256 == second.Sha256 && File.Exists(second.Path), "Keeping the kept copy itself broke it.");
            check(KeptInstaller.IsWholeSetup(second.Path, 1000) && !KeptInstaller.IsWholeSetup(second.Path, 1 << 30) &&
                !KeptInstaller.IsWholeSetup(second.Path + KeptInstaller.HashSuffix, 10) && !KeptInstaller.IsWholeSetup(null, 10),
                "IsWholeSetup accepted a file smaller than the payload or not an .exe.");
        });

        test("An installation without a kept installer says so, with Setup's reason when it gave one", () =>
        {
            var install = NewInstall(root, "none");
            var state = KeptInstaller.Find(install);
            check(state.Kept == null && state.NotKept == null, "An installation from before the feature reported an installer.");
            KeptInstaller.RecordNotKept(install, "Not enough free space on X:\\");
            check(KeptInstaller.Find(install).NotKept == "Not enough free space on X:\\", "The recorded reason was not read back.");
            reject(() => KeptInstaller.CopyTo(new KeptInstallerInfo(InstallerKind.WindowsSetup, Path.Combine(KeptInstaller.Folder(install), "HellForge-Setup-1.0.0.exe"),
                "1.0.0", 1, null), root));
        });

        test("Keeping refuses a drive without room, and makes room by removing the older copy when that is enough", () =>
        {
            var install = NewInstall(root, "space");
            var downloads = Path.Combine(root, "kept-installer", "space", "Downloads");
            var old = KeptInstaller.Keep(install, Fake(downloads, "old.exe", 3), InstallerKind.WindowsSetup, "0.4.0-beta");
            var source = Fake(downloads, "new.exe", 4);
            try
            {
                KeptInstaller.Keep(install, source, InstallerKind.WindowsSetup, "0.5.0-beta", _ => Free(1000));
                throw new Exception("A full drive was accepted.");
            }
            catch (NotEnoughSpaceException error) { check(error.Message.Contains("X:\\", StringComparison.Ordinal), "The message does not name the drive."); }
            check(File.Exists(old.Path), "A refused keep removed the older copy.");
            // room only once the older copy is gone: it is removed first, and the new one kept
            var kept = KeptInstaller.Keep(install, source, InstallerKind.WindowsSetup, "0.5.0-beta", _ => Free(DiskSpace.Margin + 100_000));
            check(File.Exists(kept.Path) && !File.Exists(old.Path) && !File.Exists(old.Path + KeptInstaller.HashSuffix), "The older copy was not replaced.");
        });

        test("Copying the kept installer checks its hash: a damaged copy is refused before anything is written", () =>
        {
            var install = NewInstall(root, "copy");
            var kept = KeptInstaller.Keep(install, Fake(Path.Combine(root, "kept-installer", "copy", "Downloads"), "setup.exe", 5), InstallerKind.WindowsSetup, "0.5.0-beta");
            var usb = Directory.CreateDirectory(Path.Combine(root, "kept-installer", "copy", "usb")).FullName;
            var copy = KeptInstaller.CopyTo(KeptInstaller.Find(install).Kept!, usb);
            check(copy.File == Path.Combine(usb, "HellForge-Setup-0.5.0-beta.exe") && Sha(copy.File) == kept.Sha256 && copy.Sha256 == kept.Sha256,
                "The copy is not the kept installer.");
            check(File.Exists(copy.File + KeptInstaller.HashSuffix), "No .sha256 beside the copy.");
            // a flipped byte in the kept file
            var bytes = File.ReadAllBytes(kept.Path);
            bytes[1234] ^= 0x40;
            File.WriteAllBytes(kept.Path, bytes);
            var other = Directory.CreateDirectory(Path.Combine(root, "kept-installer", "copy", "usb2")).FullName;
            try
            {
                KeptInstaller.CopyTo(KeptInstaller.Find(install).Kept!, other);
                throw new Exception("A damaged kept installer was copied.");
            }
            catch (InvalidDataException error) { check(error.Message.Contains("damaged", StringComparison.Ordinal), "The refusal does not say damaged: " + error.Message); }
            check(Directory.GetFileSystemEntries(other).Length == 0, "A refused copy left files behind.");
            // no .sha256: refused, since a damaged file could not be told apart
            File.Delete(kept.Path + KeptInstaller.HashSuffix);
            check(KeptInstaller.Find(install).Kept!.Sha256 == null, "A missing hash file was read as a hash.");
            reject(() => KeptInstaller.CopyTo(KeptInstaller.Find(install).Kept!, other));
        });

        test("Copying refuses a destination without room, a missing one, the installer folder itself, and names it reads unsafely", () =>
        {
            var install = NewInstall(root, "refusals");
            var kept = KeptInstaller.Keep(install, Fake(Path.Combine(root, "kept-installer", "refusals", "Downloads"), "setup.exe", 6), InstallerKind.WindowsSetup, "0.5.0-beta");
            var usb = Directory.CreateDirectory(Path.Combine(root, "kept-installer", "refusals", "usb")).FullName;
            try
            {
                KeptInstaller.CopyTo(kept, usb, space: _ => Free(100_000));
                throw new Exception("A full stick was accepted.");
            }
            catch (NotEnoughSpaceException) { }
            check(Directory.GetFileSystemEntries(usb).Length == 0, "A copy refused for space wrote something.");
            reject(() => KeptInstaller.CopyTo(kept, Path.Combine(usb, "missing")));
            reject(() => KeptInstaller.CopyTo(kept, KeptInstaller.Folder(install)));
            check(KeptInstaller.VersionForName("1.0.0/../../x (abc)") == "1.0.0....x" && KeptInstaller.VersionForName("..\\..") == "unknown" &&
                KeptInstaller.VersionForName("0.5.0-beta (19626f6)") == "0.5.0-beta", "A version made an unsafe or wrong file name.");
            // files in the installer folder that HellForge would not write are not offered
            File.WriteAllText(Path.Combine(KeptInstaller.Folder(install), "HellForge-Setup-x.zip"), "");
            File.WriteAllText(Path.Combine(KeptInstaller.Folder(install), "other.exe"), "");
            File.SetLastWriteTimeUtc(kept.Path, DateTime.UtcNow.AddDays(-1));
            check(KeptInstaller.Find(install).Kept?.Path == kept.Path, "A file HellForge does not write was offered as the installer.");
        });

        test("A kept Linux package is copied as the installer folder: the launcher files beside HellForge-Linux.zip", () =>
        {
            var install = NewInstall(root, "linux");
            var package = Path.Combine(root, "kept-installer", "linux", "installer", LinuxAppInstaller.PackageArchive);
            Directory.CreateDirectory(Path.GetDirectoryName(package)!);
            using (var archive = ZipFile.Open(package, ZipArchiveMode.Create))
            {
                foreach (var (name, text) in new[] { ("app/launcher-linux/HellForge.Launcher", "launcher"), ("app/launcher-linux/lib/libSkia.so", "skia"),
                    ("app/app-build.json", "{}"), ("HellForge.exe", "exe") })
                    using (var writer = new StreamWriter(archive.CreateEntry(name).Open())) writer.Write(text);
            }
            check(KeptInstaller.KeepLinuxPackage(install, package, "0.5.0-beta") == null, "The Linux package was not kept.");
            var kept = KeptInstaller.Find(install).Kept!;
            check(kept.Kind == InstallerKind.LinuxPackage && Path.GetFileName(kept.Path) == "HellForge-Linux-0.5.0-beta.zip", "The Linux package's kept name is wrong.");
            var usb = Directory.CreateDirectory(Path.Combine(root, "kept-installer", "linux", "usb")).FullName;
            var copy = KeptInstaller.CopyTo(kept, usb);
            var folder = Path.Combine(usb, "HellForge-Linux-Installer-0.5.0-beta");
            check(copy.Path == folder && File.ReadAllText(Path.Combine(folder, "HellForge.Launcher")) == "launcher" &&
                File.Exists(Path.Combine(folder, "lib", "libSkia.so")) && Sha(Path.Combine(folder, LinuxAppInstaller.PackageArchive)) == kept.Sha256 &&
                !File.Exists(Path.Combine(folder, "app-build.json")), "The installer folder is not the launcher files beside the package.");
            check(LinuxAppInstaller.FindPackage(folder) == Path.Combine(folder, LinuxAppInstaller.PackageArchive), "The Linux installer would not find the copied package.");
            // the package in its unpacked form (Nexus Mods' zip) is kept zipped
            var unpacked = Path.Combine(root, "kept-installer", "linux", "unpacked", LinuxAppInstaller.PackageFolder);
            Directory.CreateDirectory(Path.Combine(unpacked, "app", "launcher-linux"));
            File.WriteAllText(Path.Combine(unpacked, "app", "launcher-linux", "HellForge.Launcher"), "launcher 2");
            check(KeptInstaller.KeepLinuxPackage(install, unpacked, "0.5.1-beta") == null, "The unpacked package was not kept.");
            var zipped = KeptInstaller.Find(install).Kept!;
            using (var archive = ZipFile.OpenRead(zipped.Path))
                check(archive.GetEntry("app/launcher-linux/HellForge.Launcher") != null, "The unpacked package was not zipped with its own paths.");
            check(!File.Exists(kept.Path), "The older Linux package was not replaced.");
            // an entry that leaves the installer folder
            var evil = Path.Combine(root, "kept-installer", "linux", "evil", LinuxAppInstaller.PackageArchive);
            Directory.CreateDirectory(Path.GetDirectoryName(evil)!);
            using (var archive = ZipFile.Open(evil, ZipArchiveMode.Create))
                using (var writer = new StreamWriter(archive.CreateEntry("app/launcher-linux/../../../escaped.txt").Open())) writer.Write("x");
            KeptInstaller.KeepLinuxPackage(install, evil, "0.5.2-beta");
            var evilUsb = Directory.CreateDirectory(Path.Combine(root, "kept-installer", "linux", "usb-evil")).FullName;
            reject(() => KeptInstaller.CopyTo(KeptInstaller.Find(install).Kept!, evilUsb));
            check(!File.Exists(Path.Combine(evilUsb, "..", "escaped.txt")) && !File.Exists(Path.Combine(root, "kept-installer", "linux", "escaped.txt")),
                "An entry was written outside the installer folder.");
        });
    }
}
