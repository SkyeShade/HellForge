using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace HellForge.Core;

// Source-first mod builds (docs/sdk-roadmap.md phase 4, docs/modding/tools.md, docs/internals.md "Source mod builds"): a code mod that ships its C or C++
// sources (src/) is compiled by HellForge before the session, with the SDK's pinned Zig and the same flags on every
// machine, into <installation>/temp/mod-builds/<mod id>/, and loaded from there; a DLL in the mod's own folder is not used.
//   key     a hash of the mod's manifest and every file under src/, the SDK's headers, the compiler's identity (its
//           version and target, not its archive) and flags, and the game build: anything that changes rebuilds, so a mod
//           follows a game update by itself when its names still exist, and a native Linux build and a Windows session
//           of the same mod share one key.
//   record  record.json beside the build: its key, the sources' hash and the DLL's SHA-256. A session uses the build only
//           when the key and both hashes still match (a stale, corrupted or swapped DLL is rebuilt, never loaded): a guard
//           against accidents and casual swaps, not a determined local attacker (who could edit the sources or the record).
//   errors  a mod that does not compile stays off for that session (the session still runs); its record keeps the
//           compiler's message (file and line) for the launcher's Mods page, and the same sources are not compiled again.
// Friends compare a source mod by its sources (ModCatalog.Digest), so peers build identical mods.
public sealed record ModBuildRecord(string Key, string SourceHash, string Dll, string? DllSha256, string? Error, DateTime Built,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? Compiler = null,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? CompilerHost = null);

// Where a session's source builds are, for the launcher's progress bar (HellForge.Cli mods-stage --progress): the stage
// ("tools": fetching the compiler, "compiling", "done"), the source mods done and in all, and the one being built.
public sealed record ModBuildProgress(string Stage, int Done, int Total, string Mod);

// How a build runs: InstallTools fetches Zig when it is missing or damaged (the Linux wrapper's build-source-mods may not:
// it unpacks Zig again only from tools/downloads), and RebuildFailed compiles unchanged sources that failed (Build now).
public sealed record ModBuildOptions(bool InstallTools = true, bool RebuildFailed = false);

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

    // The build's key: the sources, the SDK headers, the compiler's identity and flags, and the game build. The
    // "hf-source-key-v2" salt changes the scheme's meaning from the compiler archive to its identity; it rebuilds old
    // caches once, then a Windows and a native Linux build of the same sources and pin share the key.
    public static string Key(string modFolder, ModSdk sdk, string buildId) => Key(SourceHash(modFolder), sdk, buildId, ModKit.Sources(modFolder).Any(f => f.EndsWith(".cpp", StringComparison.Ordinal)));

    static string Key(string sourceHash, ModSdk sdk, string buildId, bool cpp)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(Encoding.UTF8.GetBytes("hf-source-key-v2\n" + sourceHash + "\n" + buildId + "\n" + string.Join(" ", ModKit.Flags(cpp)) + "\n"));
        hash.AppendData(Encoding.UTF8.GetBytes(ModKit.CompilerIdentity(sdk) + "\n"));
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

    // What a build before the session (the Linux wrapper's build-source-mods) builds: the player's enabled mods and, when
    // the session follows a mod sync (sync: the launcher's --mod-sync), the host's mods it plays from the synced store,
    // resolved as the session's mods-stage resolves them (ModSyncSession.Resolve; sandboxed: whether the session is, so a
    // stranger's trusted mod is built only for a sandboxed one). Without them a host's source mod would stay off in that
    // session and the host refuse the join again. Why the host's are not built goes to log.
    public static List<ModManifest> ForSession(InstallLayout install, IReadOnlyList<ModManifest> catalog, bool sync, bool sandboxed, Action<string> log,
        Func<string, string, bool>? verified = null)
    {
        var active = ModCatalog.Active(catalog);
        if (!sync) return active;
        try
        {
            var (synced, problem) = ModSyncSession.Read(ModSyncSession.PathIn(install)).Resolve(install.Mods, catalog, sandboxed: sandboxed, verified: verified);
            if (synced == null) { log("Mod sync: the host's mods are not built, because " + problem); return active; }
            return [.. active.Concat(synced.Where(m => active.All(a => !string.Equals(Path.GetFullPath(a.Folder), Path.GetFullPath(m.Folder), StringComparison.Ordinal))))];
        }
        catch (Exception error) when (error is InvalidDataException or IOException or JsonException)
        {
            log("Mod sync: the host's mods are not built, because the sync file could not be read (" + error.Message + ").");
            return active;
        }
    }

    // Every enabled source mod built (or its build reused) for this session: the mods to load, with the DLL each loads,
    // and the problems (a mod that did not compile is left out). tools: on Windows, installs the SDK's pinned compiler
    // when missing; Linux installs the native compiler in the wrapper before the session.
    public static (List<ModManifest> Mods, List<string> Problems) Prepare(InstallLayout install, ModSdk? sdk, IReadOnlyList<ModManifest> mods, string buildId,
        Action<string>? log = null, ModBuildOptions? options = null, Action<ModBuildProgress>? progress = null)
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
                // Linux builds natively before the session (hellforge-linux.sh), and a Wine session uses that record: it
                // never installs or starts zig.exe. Windows keeps installing the pinned tools on first need.
                if (!OperatingSystem.IsLinux() && !ModKit.NativeBuildOnly)
                    InstallTools(sdk, options?.InstallTools ?? true, new("tools", done, total, mod.Name), log, progress);
                ready.Add(mod with { BuiltDll = Ensure(install, sdk, mod, buildId, log, options) });
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

    // The pinned tools a Windows session installs on first need. Linux installs its native Zig in the wrapper before
    // the session instead (hellforge-linux.sh), so its sessions never download or start zig.exe.
    static void InstallTools(ModSdk sdk, bool installTools, ModBuildProgress state, Action<string>? log, Action<ModBuildProgress>? progress)
    {
        if (ModKit.ToolsReady(sdk)) return;
        if (!installTools) throw new InvalidDataException("the mod tools are not installed (the launcher's Mods page: Install mod tools).");
        if (LinuxNetworkBlock.Requested)
            throw new InvalidDataException("the mod tools (Zig) are not installed, and the session has no network to fetch them: they could not be fetched " +
                "before the session; install them from the launcher's Mods page (Install mod tools), then start the session again.");
        log?.Invoke("Installing the mod tools (HellForge's pinned Zig) to build source mods...");
        progress?.Invoke(state);
        ModKit.InstallToolsAsync(sdk, ide: false, line => log?.Invoke(line)).GetAwaiter().GetResult();
    }

    // The mod's DLL for this build: its cached build when the record and both hashes match, else built now (ticket 0353:
    // a failure in Zig's own files, not the mod's, unpacks Zig again and builds once more, and is never kept).
    public static string Ensure(InstallLayout install, ModSdk sdk, ModManifest mod, string buildId, Action<string>? log = null, ModBuildOptions? options = null)
    {
        options ??= new();
        var folder = Directory.CreateDirectory(ModFolder(install, mod.Id)).FullName;
        var sourceHash = SourceHash(mod.Folder);
        var key = Key(sourceHash, sdk, buildId, ModKit.Sources(mod.Folder).Any(f => f.EndsWith(".cpp", StringComparison.Ordinal)));
        var target = new Target(folder, key, sourceHash, Path.Combine(folder, key[..16], mod.CodeDll!));
        if (UseKept(target, Record(install, mod.Id), sdk, mod.Id, options.RebuildFailed, log)) return target.Dll;
        log?.Invoke($"{mod.Id}: building from source ({ModKit.Sources(mod.Folder).Count} file(s))...");
        return BuildRepairing(target, sdk, mod, options.InstallTools, log);
    }

    // One build's place: the mod's build folder, its key and sources' hash, and where its DLL goes.
    sealed record Target(string Folder, string Key, string SourceHash, string Dll);

    // Whether the kept build serves: its DLL when the record matches it; a kept failure is thrown again, unless
    // rebuildFailed (Build now), or it failed in Zig's own files, or those are damaged now.
    static bool UseKept(Target target, ModBuildRecord? record, ModSdk sdk, string id, bool rebuildFailed, Action<string>? log)
    {
        if (record == null || record.Key != target.Key || record.SourceHash != target.SourceHash) return false;
        if (record.Error == null)
        {
            if (File.Exists(target.Dll) && Sha256(target.Dll) == record.DllSha256) return true;
            log?.Invoke($"{id}: its cached build does not match its record; building it again.");
            return false;
        }
        if (rebuildFailed) log?.Invoke($"{id}: building it again, though its sources are unchanged since it did not compile.");
        else if (ToolsFailure(record.Error) || PinnedCompiler(sdk) is { } pinned && ModKit.ToolDamage(sdk, pinned) != null)
            log?.Invoke($"{id}: its last build failed in the mod tools (Zig), not in the mod; building it again.");
        else throw new InvalidDataException("did not compile (sources unchanged since):\n" + record.Error);
        return false;
    }

    // The build, and when it failed in Zig's own files, Zig unpacked again and one more build (AfterFailure).
    static string BuildRepairing(Target target, ModSdk sdk, ModManifest mod, bool allowDownload, Action<string>? log)
    {
        for (var repaired = false; ; repaired = true)
        {
            try { return Compile(target, sdk, mod, log); }
            catch (InvalidDataException error) { AfterFailure(target, sdk, mod, error.Message, repaired, allowDownload, log); }
        }
    }

    // A failed build: the mod's own failure is kept and thrown. One in Zig's own files (its message, or its file list
    // finding a file gone) unpacks Zig again for one more build, or after that is thrown without being kept.
    static void AfterFailure(Target target, ModSdk sdk, ModManifest mod, string message, bool repaired, bool allowDownload, Action<string>? log)
    {
        var tool = PinnedCompiler(sdk);
        var damage = tool == null ? null : ModKit.ToolDamage(sdk, tool);
        var zigs = ToolsFailure(message);
        if (damage == null && !zigs)
        {
            Write(target.Folder, NewRecord(target.Key, target.SourceHash, mod.CodeDll!, null, Trim(message), sdk));
            throw new InvalidDataException("did not compile:\n" + Trim(message));
        }
        var why = damage ?? "Zig could not build its own libraries";
        if (tool == null || repaired) throw new InvalidDataException(Damaged(why, message));
        log?.Invoke($"{mod.Id}: {(zigs ? "the build failed in the mod tools, not in the mod" : "did not compile, and the mod tools need checking first")} " +
            $"({why}): unpacking {tool.Name} again, then building again...");
        try { ModKit.RepairToolAsync(sdk, tool, allowDownload, line => log?.Invoke(line)).GetAwaiter().GetResult(); }
        catch (Exception failure) when (failure is InvalidDataException or IOException or UnauthorizedAccessException or HttpRequestException or TaskCanceledException)
        {
            throw new InvalidDataException(Damaged(why, message) + "\nUnpacking Zig again did not work: " + failure.Message);
        }
    }

    // One compile into a staging folder, moved into place with its record; the compiler's failure is thrown as it came.
    static string Compile(Target target, ModSdk sdk, ModManifest mod, Action<string>? log)
    {
        var staging = Path.Combine(target.Folder, "building-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(staging);
        try
        {
            ModKit.Build(mod.Folder, sdk, line => log?.Invoke(line), Path.Combine(staging, mod.CodeDll!));
            var built = Path.GetDirectoryName(target.Dll)!;
            if (Directory.Exists(built)) Directory.Delete(built, recursive: true);
            Directory.Move(staging, built);
            Write(target.Folder, NewRecord(target.Key, target.SourceHash, mod.CodeDll!, Sha256(target.Dll), null, sdk));
            foreach (var old in Directory.GetDirectories(target.Folder).Where(d => Path.GetFileName(d) != target.Key[..16]))
                try { Directory.Delete(old, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { } /* a DLL a running game holds: next time */
            log?.Invoke($"{mod.Id}: built ({new FileInfo(target.Dll).Length / 1024} KB).");
            return target.Dll;
        }
        finally
        {
            if (Directory.Exists(staging)) try { Directory.Delete(staging, recursive: true); } catch (IOException) { }
        }
    }

    // Zig failed building its own runtime libraries (compiler_rt, libzigc, MinGW's C runtime, libc++), which it compiles
    // from its lib folder before any of the mod's code: never the mod's fault. (Zig 0.17.0's wording.)
    public static bool ToolsFailure(string compilerOutput) => compilerOutput.Contains("error: sub-compilation of ", StringComparison.Ordinal);

    static string Damaged(string why, string compilerOutput) =>
        $"did not compile, because the mod tools (Zig) are damaged, not the mod: {why}. An antivirus that removes files from the HellForge " +
        "folder does this: exclude the folder, then start the session again.\n" + Trim(compilerOutput);

    // The pinned tool this host compiles with (tools/zig/zig.exe, or tools/zig-linux/zig), or null for a Zig HellForge did
    // not unpack (ZIG or PATH on Linux), which it neither checks nor repairs.
    static ModTool? PinnedCompiler(ModSdk sdk)
    {
        string compiler;
        try { compiler = Path.GetFullPath(ModKit.Compiler(sdk, OperatingSystem.IsLinux(), ModKit.NativeBuildOnly)); }
        catch (IOException) { return null; }
        return ModKit.ToolsForThisPlatform(sdk).FirstOrDefault(t => Path.GetFullPath(ModKit.ToolPath(sdk, t)) == compiler);
    }

    // The compiler's message, without the "did not compile" heading and the mod folder's own path (file:line stays).
    static string Trim(string message)
    {
        var text = message.StartsWith("The mod did not compile:\n", StringComparison.Ordinal) ? message["The mod did not compile:\n".Length..] : message;
        var lines = text.Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).Take(40);
        return string.Join("\n", lines);
    }

    // A native Linux build says which host and compiler made it; a Windows build keeps the record it had.
    static ModBuildRecord NewRecord(string key, string sourceHash, string dll, string? sha256, string? error, ModSdk sdk)
    {
        var host = OperatingSystem.IsLinux() ? ModKit.HostPlatform : null;
        return new(key, sourceHash, dll, sha256, error, DateTime.UtcNow, host == null ? null : ModKit.CompilerIdentity(sdk), host);
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
