using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// Source-first mod builds (docs/sdk-roadmap.md phase 4, docs/modding/tools.md, docs/internals.md "Source mod builds"): a code mod that ships its C or C++
// sources (src/) is compiled by HellForge before the session, with the SDK's pinned Zig and the same flags on every
// machine, into <installation>/temp/mod-builds/<mod id>/, and loaded from there; a DLL in the mod's own folder is not used.
//   key     a hash of the mod's manifest and every file under src/, the SDK's headers, the compiler pin and flags, and the
//           game build: anything that changes rebuilds, so a mod follows a game update by itself when its names still exist.
//   record  record.json beside the build: its key, the sources' hash and the DLL's SHA-256. A session uses the build only
//           when the key and both hashes still match (a stale, corrupted or swapped DLL is rebuilt, never loaded): a guard
//           against accidents and casual swaps, not a determined local attacker (who could edit the sources or the record).
//   errors  a mod that does not compile stays off for that session (the session still runs); its record keeps the
//           compiler's message (file and line) for the launcher's Mods page, and the same sources are not compiled again.
// Friends compare a source mod by its sources (ModCatalog.Digest), so peers build identical mods.
public sealed record ModBuildRecord(string Key, string SourceHash, string Dll, string? DllSha256, string? Error, DateTime Built);

// Where a session's source builds are, for the launcher's progress bar (HellForge.Cli mods-stage --progress): the stage
// ("tools": fetching the compiler, "compiling", "done"), the source mods done and in all, and the one being built.
public sealed record ModBuildProgress(string Stage, int Done, int Total, string Mod);

public static class ModBuilds
{
    public const string RecordName = "record.json";
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public static string Folder(InstallLayout install) => Path.Combine(install.Temp, "mod-builds");
    static string ModFolder(InstallLayout install, string id) => Path.Combine(Folder(install), id);

    // The files a source mod is built from: its manifest and every file under src/ (headers too), in a fixed order.
    public static List<string> SourceFiles(string modFolder)
    {
        var files = new List<string>();
        if (ModCatalog.ManifestIn(modFolder) is { } manifest) files.Add(manifest);
        var src = Path.Combine(modFolder, "src");
        if (Directory.Exists(src))
            files.AddRange(Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories)
                .Where(f => !Path.GetRelativePath(src, f).Split(Path.DirectorySeparatorChar).Any(p => p.StartsWith('.') || p.StartsWith("~$", StringComparison.Ordinal)))
                .Order(StringComparer.Ordinal));
        return files;
    }

    // The sources' hash: each file's path (relative, '/') and contents.
    public static string SourceHash(string modFolder)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var file in SourceFiles(modFolder))
        {
            hash.AppendData(Encoding.UTF8.GetBytes(Path.GetRelativePath(modFolder, file).Replace('\\', '/') + "\n"));
            hash.AppendData(ModCatalog.ReadShared(file));
            hash.AppendData("\n"u8);
        }
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }

    // The build's key: the sources, the SDK headers, the compiler (its pinned archive) and flags, and the game build.
    public static string Key(string modFolder, ModSdk sdk, string buildId) => Key(SourceHash(modFolder), sdk, buildId, ModKit.Sources(modFolder).Any(f => f.EndsWith(".cpp", StringComparison.Ordinal)));

    static string Key(string sourceHash, ModSdk sdk, string buildId, bool cpp)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(Encoding.UTF8.GetBytes(sourceHash + "\n" + buildId + "\n" + string.Join(" ", ModKit.Flags(cpp)) + "\n"));
        hash.AppendData(Encoding.UTF8.GetBytes((ModKit.Tools(sdk).FirstOrDefault(t => t.Name == "zig")?.Sha256 ?? "no zig") + "\n"));
        foreach (var header in Directory.EnumerateFiles(sdk.Folder, "*.h").Order(StringComparer.Ordinal))
        {
            hash.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(header) + "\n"));
            hash.AppendData(File.ReadAllBytes(header));
        }
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }

    // The mod's last build, or null.
    public static ModBuildRecord? Record(InstallLayout install, string id)
    {
        var path = Path.Combine(ModFolder(install, id), RecordName);
        try { return File.Exists(path) ? JsonSerializer.Deserialize<ModBuildRecord>(File.ReadAllText(path)) : null; }
        catch (Exception error) when (error is JsonException or IOException) { return null; }
    }

    // Every enabled source mod built (or its build reused) for this session: the mods to load, with the DLL each loads,
    // and the problems (a mod that did not compile is left out). tools: installs the SDK's pinned compiler when missing.
    public static (List<ModManifest> Mods, List<string> Problems) Prepare(InstallLayout install, ModSdk? sdk, IReadOnlyList<ModManifest> mods, string buildId,
        Action<string>? log = null, bool installTools = true, Action<ModBuildProgress>? progress = null)
    {
        var ready = new List<ModManifest>();
        var problems = new List<string>();
        int total = mods.Count(m => m.HasSource), done = 0;
        foreach (var mod in mods)
        {
            if (!mod.HasSource) { ready.Add(mod); continue; }
            try
            {
                progress?.Invoke(new("compiling", done, total, mod.Name)); // the name players see, for the launcher's bar
                if (sdk == null) throw new InvalidDataException("the mod SDK is missing from this installation (app/sdk); reinstall HellForge.");
                if (!ModKit.ToolsReady(sdk))
                {
                    if (!installTools) throw new InvalidDataException("the mod tools are not installed (the launcher's Mods page: Install mod tools).");
                    // Linux: the session is in a network namespace with no way out, where the download is refused (issue
                    // 20); hellforge-linux.sh fetches them before it enters the namespace.
                    if (LinuxNetworkBlock.Requested)
                        throw new InvalidDataException("the mod tools (Zig) are not installed, and the session has no network to fetch them: start HellForge " +
                            "with app/linux/hellforge-linux.sh, which fetches them first.");
                    log?.Invoke("Installing the mod tools (HellForge's pinned Zig) to build source mods...");
                    progress?.Invoke(new("tools", done, total, mod.Name));
                    ModKit.InstallToolsAsync(sdk, ide: false, line => log?.Invoke(line)).GetAwaiter().GetResult();
                }
                ready.Add(mod with { BuiltDll = Ensure(install, sdk, mod, buildId, log) });
            }
            catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or HttpRequestException or
                System.ComponentModel.Win32Exception)
            {
                problems.Add($"{mod.Id}: {error.Message}");
            }
            done++;
        }
        progress?.Invoke(new("done", done, total, ""));
        return (ready, problems);
    }

    // The mod's DLL for this build: its cached build when the record and both hashes match, else built now.
    public static string Ensure(InstallLayout install, ModSdk sdk, ModManifest mod, string buildId, Action<string>? log = null)
    {
        var folder = Directory.CreateDirectory(ModFolder(install, mod.Id)).FullName;
        var sourceHash = SourceHash(mod.Folder);
        var key = Key(sourceHash, sdk, buildId, ModKit.Sources(mod.Folder).Any(f => f.EndsWith(".cpp", StringComparison.Ordinal)));
        var record = Record(install, mod.Id);
        var dll = Path.Combine(folder, key[..16], mod.CodeDll!);
        if (record != null && record.Key == key && record.SourceHash == sourceHash)
        {
            if (record.Error != null) throw new InvalidDataException("did not compile (sources unchanged since):\n" + record.Error);
            if (File.Exists(dll) && Sha256(dll) == record.DllSha256) return dll;
            log?.Invoke($"{mod.Id}: its cached build does not match its record; building it again.");
        }
        log?.Invoke($"{mod.Id}: building from source ({ModKit.Sources(mod.Folder).Count} file(s))...");
        var staging = Path.Combine(folder, "building-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(staging);
        try
        {
            var built = ModKit.Build(mod.Folder, sdk, line => log?.Invoke(line), Path.Combine(staging, mod.CodeDll!));
            var target = Path.GetDirectoryName(dll)!;
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(staging, target);
            Write(folder, new ModBuildRecord(key, sourceHash, mod.CodeDll!, Sha256(dll), null, DateTime.UtcNow));
            foreach (var old in Directory.GetDirectories(folder).Where(d => Path.GetFileName(d) != key[..16]))
                try { Directory.Delete(old, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { } /* a DLL a running game holds: next time */
            log?.Invoke($"{mod.Id}: built ({new FileInfo(dll).Length / 1024} KB).");
            return dll;
        }
        catch (InvalidDataException error)
        {
            Write(folder, new ModBuildRecord(key, sourceHash, mod.CodeDll!, null, Trim(error.Message), DateTime.UtcNow));
            throw new InvalidDataException("did not compile:\n" + Trim(error.Message));
        }
        finally
        {
            if (Directory.Exists(staging)) try { Directory.Delete(staging, recursive: true); } catch (IOException) { }
        }
    }

    // The compiler's message, without the "did not compile" heading and the mod folder's own path (file:line stays).
    static string Trim(string message)
    {
        var text = message.StartsWith("The mod did not compile:\n", StringComparison.Ordinal) ? message["The mod did not compile:\n".Length..] : message;
        var lines = text.Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).Take(40);
        return string.Join("\n", lines);
    }

    static void Write(string folder, ModBuildRecord record)
    {
        var path = Path.Combine(folder, RecordName);
        File.WriteAllText(path + ".partial", JsonSerializer.Serialize(record, Json));
        File.Move(path + ".partial", path, overwrite: true);
    }

    static string Sha256(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(file));
    }
}
