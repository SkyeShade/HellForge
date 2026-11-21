using System.Text.Json;
using HellForge.Core;

// Where a source mod's build stands (ModBuilds.Status), as the Mods page and HellForge.Cli's mods show it. The states
// that need a compiled build (built, did-not-compile) are covered with Zig in ModKitTests' source mod build.
static class ModBuildStatusTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A source mod's build reads as not built, changed (keeping its old error) or failed in the tools; other mods have none", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var install = new InstallLayout(Path.Combine(root, "build-status"));
            install.Create("12345");
            var folder = ModKit.Create(repository, install.Mods, "tester.status-mod", "Status Mod");
            var mod = ModCatalog.Parse(folder, EntityProfile.Current);
            check(ModBuilds.Status(install, mod with { HasSource = false }, repository, true) == null, "A mod without sources has a build state.");
            check(ModBuilds.Status(install, mod, repository, true)?.State == "not-built", "A mod never built is not 'not-built'.");

            void Record(string? error)
            {
                var cache = Directory.CreateDirectory(Path.Combine(ModBuilds.Folder(install), mod.Id)).FullName;
                File.WriteAllText(Path.Combine(cache, ModBuilds.RecordName), JsonSerializer.Serialize(
                    new ModBuildRecord("an old key", "old sources", "status_mod.dll", null, error, new DateTime(2026, 11, 1, 12, 0, 0, DateTimeKind.Utc))));
            }
            Record(null);
            var changed = ModBuilds.Status(install, mod, repository, true);
            check(changed is { State: "changed", Error: null } && changed.Built == new DateTime(2026, 11, 1, 12, 0, 0, DateTimeKind.Utc), "Another key is not 'changed'.");
            Record("src/status_mod.c:3: error: use of undeclared identifier 'missing'");
            check(ModBuilds.Status(install, mod, repository, true) is { State: "changed", Error: not null }, "A failed build with changed sources lost its error.");
            Record("error: sub-compilation of compiler_rt failed");
            check(ModBuilds.Status(install, mod, repository, true)?.State == "tools-failed", "A failure in Zig's own libraries is not 'tools-failed'.");
        });
    }
}
