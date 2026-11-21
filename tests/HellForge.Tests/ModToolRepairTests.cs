using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using HellForge.Core;

// Ticket 0353: an antivirus took files from Zig's lib\std, every build failed in Zig's own libraries, and the failure was
// kept as the mod's, so restoring Zig changed nothing. The tools now keep a list of their files, a failure in Zig's own
// files unpacks Zig again and builds once more, and only the mod's own failures are kept.
static class ModToolRepairTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A pinned tool keeps a list of its files: one with a file gone or changed is reported and unpacked again", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "tool-list")).FullName;
            var bytes = Zip(("zig-x/zig.exe", "exe"), ("zig-x/lib/std/Io.zig", "pub const Io = struct {};"), ("zig-x/lib/std/BitStack.zig", "bits"));
            var tool = new ModTool("zig", "0.17.0", "https://example.invalid/zig-x.zip", Convert.ToHexString(SHA256.HashData(bytes)), "zig.exe", false,
                "windows-x64", bytes.Length);
            var sdk = new ModSdk(folder, Path.Combine(folder, "tools"), folder, folder);
            var fetches = new Counting(bytes);
            using var http = new HttpClient(fetches);
            ModKit.InstallToolAsync(http, sdk, tool, _ => { }, CancellationToken.None).GetAwaiter().GetResult();
            var unpacked = Path.Combine(sdk.Tools, "zig");
            check(ModKit.ToolDamage(sdk, tool) == null && fetches.Count == 1, "Installed whole, from one download.");

            File.Delete(Path.Combine(unpacked, "lib", "std", "Io.zig"));
            check(ModKit.ToolDamage(sdk, tool) is { } gone && gone.Contains("1 of zig's 3 files") && gone.Contains("lib/std/Io.zig"),
                "A file gone is named: " + ModKit.ToolDamage(sdk, tool));
            File.WriteAllText(Path.Combine(unpacked, "lib", "std", "BitStack.zig"), "changed bits");
            check(ModKit.ToolDamage(sdk, tool) is { } changed && changed.Contains("2 of zig's 3 files"), "A file of another size counts too.");

            // Install mod tools no longer calls a damaged tool installed: it unpacks the kept archive again.
            ModKit.InstallToolAsync(http, sdk, tool, _ => { }, CancellationToken.None).GetAwaiter().GetResult();
            check(ModKit.ToolDamage(sdk, tool) == null && File.Exists(Path.Combine(unpacked, "lib", "std", "Io.zig")) && fetches.Count == 1,
                "Unpacked again from tools/downloads, without a second download.");

            File.Delete(Path.Combine(unpacked, ".hellforge-files"));
            check(ModKit.ToolDamage(sdk, tool) == null, "A tool unpacked before the list cannot be checked, so it is not called damaged.");
            var cache = Directory.CreateDirectory(ModKit.ZigCache(sdk, linuxHost: false)).FullName;
            File.WriteAllText(Path.Combine(cache, "libzigc.a"), "a half-built library");
            ModKit.RepairToolAsync(sdk, tool, allowDownload: false, _ => { }).GetAwaiter().GetResult();
            check(ModKit.ToolDamage(sdk, tool) == null && !Directory.Exists(cache), "Repaired from the kept archive with no network, and Zig's cache emptied.");

            File.Delete(Path.Combine(sdk.Tools, "downloads", "zig-x.zip"));
            var refused = "";
            try { ModKit.RepairToolAsync(sdk, tool, allowDownload: false, _ => { }).GetAwaiter().GetResult(); }
            catch (InvalidDataException error) { refused = error.Message; }
            check(refused.Contains("Install mod tools") && File.Exists(Path.Combine(unpacked, "zig.exe")),
                "Without its archive and without the network, a repair says what to do and leaves the tool as it was: " + refused);
        });

        test("A failure in Zig's own libraries is told from the mod's own errors (Zig 0.17.0's wording)", () =>
        {
            // what Zig 0.17.0 printed with files gone from its lib folder (lib/std/Io.zig, lib/libc/mingw/crt/crtdll.c)
            foreach (var tools in new[] { "error: sub-compilation of compiler_rt failed", "error: sub-compilation of libzigc failed",
                         "error: sub-compilation of mingw-w64 dllcrt2.o failed\n    note: checking cache failed: file_open FileNotFound zig/lib/libc/mingw/crt/crtdll.c" })
                check(ModBuilds.ToolsFailure(tools + "\n    zig/lib/std/Io.zig:1:1: note: unable to load \"Io.zig\": FileNotFound"), "Not seen as the tools': " + tools);
            // and what a mod's own mistakes print: a compile error, a header that is not there, a link error
            foreach (var mods in new[] { "src/mod.c:2:22: error: use of undeclared identifier 'undefined_thing'",
                         "src/mod.c:1:10: fatal error: 'missing.h' file not found", "lld-link: error: undefined symbol: missing" })
                check(!ModBuilds.ToolsFailure(mods), "Seen as the tools': " + mods);
        });

        test("A source build that fails in Zig's own files unpacks Zig again and builds; only the mod's own failure is kept", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: the stand-in compiler is a shell script)"); return; }
            var install = new InstallLayout(Directory.CreateDirectory(Path.Combine(root, "repair-install")).FullName);
            install.Create("25480438");
            var sdkFolder = Directory.CreateDirectory(Path.Combine(install.App, "sdk")).FullName;
            File.WriteAllText(Path.Combine(sdkFolder, "hellforge_mod.h"), "/* fixture */\n");
            var runs = Path.Combine(root, "repair-zig-runs.txt");
            // Zig as it fails without lib/std/Io.zig (its 0.17.0 message), else a compiler that fails on a source saying "broken"
            var script = $$"""
                #!/bin/sh
                here=$(dirname "$0")
                echo run >> "{{runs}}"
                if [ ! -f "$here/lib/std/Io.zig" ]; then
                  echo 'error: sub-compilation of libzigc failed' >&2
                  echo '    tools/zig-linux/lib/std/Io.zig:1:1: note: unable to load "Io.zig": FileNotFound' >&2
                  exit 1
                fi
                out=""; previous=""; broken=""
                for argument in "$@"; do
                  if [ "$previous" = "-o" ]; then out="$argument"; fi
                  case "$argument" in *.c) if grep -q broken "$argument"; then broken="$argument"; fi;; esac
                  previous="$argument"
                done
                if [ -n "$broken" ]; then echo "$broken:1:1: error: use of undeclared identifier 'broken'" >&2; exit 1; fi
                printf 'stand-in build' > "$out"
                """;
            var bytes = Zip(("zig-fake/zig", script), ("zig-fake/lib/std/Io.zig", "pub const Io = struct {};"));
            var sha = Convert.ToHexString(SHA256.HashData(bytes));
            File.WriteAllText(Path.Combine(sdkFolder, "tools.json"), $$"""
                {"format":1,"tools":[{"name":"zig-linux","version":"0.17.0","url":"https://example.invalid/zig-fake.zip","sha256":"{{sha}}","bytes":{{bytes.Length}},"exe":"zig","ide":false,"platform":"linux-x64"}]}
                """);
            var sdk = new ModSdk(sdkFolder, Path.Combine(install.Root, "tools"), sdkFolder, sdkFolder);
            var tool = ModKit.Tools(sdk).Single();
            using (var http = new HttpClient(new Counting(bytes))) ModKit.InstallToolAsync(http, sdk, tool, _ => { }, CancellationToken.None).GetAwaiter().GetResult();
            var folder = Directory.CreateDirectory(Path.Combine(install.Mods, "repair-mod", "src")).Parent!.FullName;
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName),
                "{\"format\":1,\"id\":\"test.repair\",\"name\":\"Repair\",\"version\":\"1\",\"code\":{\"dll\":\"repair.dll\",\"sdk\":" + ModCatalog.SdkVersion + "}}");
            var source = Path.Combine(folder, "src", "repair.c");
            File.WriteAllText(source, "int ok(void) { return 1; }\n");
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            const string build = "25480438";
            int Runs() => File.Exists(runs) ? File.ReadAllLines(runs).Length : 0;
            var io = Path.Combine(sdk.Tools, "zig-linux", "lib", "std", "Io.zig");

            // what an antivirus did: Zig's own file gone
            File.Delete(io);
            var log = new List<string>();
            var dll = ModBuilds.Ensure(install, sdk, mod, build, log.Add);
            check(File.Exists(dll) && ModBuilds.Record(install, mod.Id)!.Error == null && Runs() == 2 && File.Exists(io) &&
                  log.Any(l => l.Contains("failed in the mod tools, not in the mod")),
                "Zig was unpacked again and the mod built on the second try: " + string.Join(" | ", log));

            // a record HellForge 0.4.0 kept for Zig's own failure: built again, not shown again
            File.WriteAllText(Path.Combine(ModBuilds.Folder(install), mod.Id, ModBuilds.RecordName), JsonSerializer.Serialize(new ModBuildRecord(
                ModBuilds.Key(folder, sdk, build), ModBuilds.SourceHash(folder), "repair.dll", null, "error: sub-compilation of libzigc failed", DateTime.UtcNow)));
            check(ModBuilds.Ensure(install, sdk, mod, build) == dll && ModBuilds.Record(install, mod.Id)!.Error == null && Runs() == 3,
                "A kept failure in Zig's own libraries is built again.");

            // the mod's own mistake: kept, and shown again without compiling until Build now
            File.WriteAllText(source, "int ok(void) { return broken; }\n");
            var failed = "";
            try { ModBuilds.Ensure(install, sdk, mod, build); } catch (InvalidDataException error) { failed = error.Message; }
            check(failed.Contains("did not compile") && failed.Contains("broken") && ModBuilds.Record(install, mod.Id)!.Error is { } kept &&
                  kept.Contains("broken") && Runs() == 4, "The mod's own error is kept: " + failed);
            reject(() => ModBuilds.Ensure(install, sdk, mod, build));
            check(Runs() == 4, "Unchanged sources that did not compile are not compiled again.");
            reject(() => ModBuilds.Ensure(install, sdk, mod, build, options: new(RebuildFailed: true)));
            check(Runs() == 5, "Build now compiles them again.");

            // Zig damaged again, its archive gone, and no download allowed (a session's build-source-mods): nothing kept
            File.WriteAllText(source, "int ok(void) { return 2; }\n");
            File.Delete(io);
            File.Delete(Path.Combine(sdk.Tools, "downloads", "zig-fake.zip"));
            failed = "";
            try { ModBuilds.Ensure(install, sdk, mod, build, options: new(InstallTools: false)); } catch (InvalidDataException error) { failed = error.Message; }
            check(failed.Contains("the mod tools (Zig) are damaged, not the mod") && failed.Contains("Install mod tools") &&
                  ModBuilds.Record(install, mod.Id)!.Error is { } stays && !stays.Contains("sub-compilation"),
                "Reported as Zig's damage, and not kept as the mod's failure: " + failed);
        });
    }

    static byte[] Zip(params (string Name, string Text)[] files)
    {
        using var memory = new MemoryStream();
        using (var archive = new ZipArchive(memory, ZipArchiveMode.Create, leaveOpen: true))
            foreach (var (name, text) in files)
                using (var entry = new StreamWriter(archive.CreateEntry(name).Open())) entry.Write(text.Replace("\r\n", "\n"));
        return memory.ToArray();
    }

    sealed class Counting(byte[] bytes) : HttpMessageHandler
    {
        public int Count;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Count++;
            return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
        }
    }
}
