using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace HellForge.Core;

public static class EnginePackageInstaller
{
    public static void Install(InstallLayout install, string? archivePath = null)
    {
        archivePath ??= Path.Combine(install.App, "engine-package.zip");
        if (!File.Exists(archivePath)) throw new InvalidDataException("The engine package is missing (" + archivePath + "). Update HellForge before setup.");

        using var archive = ZipFile.OpenRead(archivePath);
        var metadata = archive.GetEntry("engine.json") ?? throw new InvalidDataException("The engine package has no engine.json.");
        using var metadataStream = metadata.Open();
        using var document = JsonDocument.Parse(metadataStream);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("build", out var build) ||
            build.ValueKind != JsonValueKind.String || build.GetString() != SupportedBuild.Current.BuildId || !root.TryGetProperty("files", out var files) ||
            files.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("The engine package does not match this HellForge build.");

        var staged = Path.Combine(install.Temp, "engine-staging");
        var backup = Path.Combine(install.Temp, "engine-backup");
        if (Directory.Exists(staged)) Directory.Delete(staged, recursive: true);
        if (Directory.Exists(backup))
            throw new InvalidDataException("A previous engine update could not be rolled back; preserve and inspect " + backup + " before retrying.");
        Directory.CreateDirectory(staged);
        var preserveBackup = false;
        try
        {
            var expected = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var file in files.EnumerateObject())
            {
                if (!IsSafeRelative(file.Name) || file.Value.ValueKind != JsonValueKind.String)
                    throw new InvalidDataException("The engine package has an invalid file entry: " + file.Name);
                if (!expected.TryAdd(file.Name, file.Value.GetString()!))
                    throw new InvalidDataException("The engine package contains a duplicate file entry: " + file.Name);
            }

            var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.Ordinal);
            foreach (var entry in archive.Entries.Where(entry => entry.FullName != "engine.json"))
                if (!entries.TryAdd(entry.FullName, entry))
                    throw new InvalidDataException("The engine package contains a duplicate file: " + entry.FullName);
            if (expected.Count == 0 || expected.Keys.Any(name => !entries.ContainsKey(name)) ||
                entries.Keys.Any(name => !expected.ContainsKey(name)))
                throw new InvalidDataException("The engine package file list does not match its contents.");

            foreach (var (name, hash) in expected)
            {
                var path = Path.GetFullPath(Path.Combine(staged, name));
                if (!LocalInstall.Within(path, staged)) throw new InvalidDataException("The engine package contains an unsafe path.");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using (var input = entries[name].Open())
                using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    input.CopyTo(output);
                using var stream = File.OpenRead(path);
                if (!Convert.ToHexString(SHA256.HashData(stream)).Equals(hash, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The engine package is damaged: " + name);
            }
            var packageMetadata = Path.Combine(staged, "engine.json");
            using (var input = metadata.Open())
            using (var output = File.Create(packageMetadata)) input.CopyTo(output);

            var folders = new[] { "session", "backend", "sdk-shims", "scripts" };
            var targets = folders.Select(folder => (Folder: folder, Current: Path.Combine(install.App, folder),
                Replacement: Path.Combine(staged, folder), Backup: Path.Combine(backup, folder))).ToArray();
            if (targets.Any(target => !Directory.Exists(target.Replacement)))
                throw new InvalidDataException("The engine package is missing a required folder.");

            Directory.CreateDirectory(backup);
            var movedCurrent = new List<(string Current, string Backup)>();
            var movedReplacement = new List<(string Current, string Replacement)>();
            try
            {
                foreach (var target in targets)
                    if (Directory.Exists(target.Current))
                    {
                        Directory.Move(target.Current, target.Backup);
                        movedCurrent.Add((target.Current, target.Backup));
                    }
                foreach (var target in targets)
                {
                    Directory.Move(target.Replacement, target.Current);
                    movedReplacement.Add((target.Current, target.Replacement));
                }
                File.Copy(packageMetadata, Path.Combine(install.App, "engine.json"), overwrite: true);
            }
            catch (Exception updateError)
            {
                try
                {
                    foreach (var (current, replacement) in movedReplacement.AsEnumerable().Reverse())
                        if (Directory.Exists(current)) Directory.Move(current, replacement);
                    foreach (var (current, old) in movedCurrent.AsEnumerable().Reverse())
                        if (Directory.Exists(old)) Directory.Move(old, current);
                }
                catch (Exception rollbackError)
                {
                    preserveBackup = true;
                    throw new IOException("The engine update failed and rollback was incomplete; the previous engine is preserved in " + backup + ".",
                        new AggregateException(updateError, rollbackError));
                }
                throw;
            }
        }
        finally
        {
            if (Directory.Exists(staged)) Directory.Delete(staged, recursive: true);
            if (!preserveBackup && Directory.Exists(backup)) Directory.Delete(backup, recursive: true);
        }
    }

    static bool IsSafeRelative(string path) =>
        path.Length > 0 && !Path.IsPathRooted(path) && !path.Contains('\\') && !path.Contains(':') && !path.Contains('\0') &&
        path.Split('/').All(part => part.Length > 0 && part is not "." and not "..");
}
