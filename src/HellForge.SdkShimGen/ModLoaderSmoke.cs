using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using HellForge.Core;

namespace HellForge.SdkShimGen;

// The mod loader end to end without the game: a fake game module (decode pin, network configuration holder, a function
// to hook), a synthetic entity region and network field type table, and the real chain - the steam_api64.dll stand-in
// starts hellforge_modloader.dll, which loads the real Machine Gun 10k code mod and a hook-test mod. The test then "finishes
// loading" the engine (fills the configuration holder) and checks every effect: events, the widened field type, the
// edited deposit record, the pattern scan and the detour.
public static unsafe class ModLoaderSmoke
{
    const ulong Backpack = 0x056DE1C5E21E723E;

    public static int Run(string standIns, string testDirectory, string machineGunMod, string? compendiumMod = null)
    {
        var root = Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "hellforge-modloader-smoke-" + Guid.NewGuid().ToString("N"))).FullName;
        var problems = new List<string>();
        void Expect(bool condition, string description) { if (!condition) problems.Add(description); }

        // The fake game.
        File.Copy(Path.Combine(testDirectory, "fake_game.dll"), Path.Combine(root, "fake_game.dll"));
        var game = NativeLibrary.Load(Path.Combine(root, "fake_game.dll"));
        var holder = NativeLibrary.GetExport(game, "hellforge_fake_network_holder");
        var pin = NativeLibrary.GetExport(game, "hellforge_fake_pin");
        var target = (delegate* unmanaged<int, int>)NativeLibrary.GetExport(game, "hellforge_fake_target");
        Expect(target(5) == 16, "The fake target works before hooking");

        // The engine's field types (not yet "loaded"): bool, then deposit_value as the game has it.
        var rows = (byte*)NativeMemory.AllocZeroed(2 * 0x18);
        void Row(int index, string name, byte kind, byte bits)
        {
            *(uint*)(rows + index * 0x18) = EngineHash.Id32(name);
            rows[index * 0x18 + 12] = kind;
            rows[index * 0x18 + 13] = bits;
        }
        Row(0, "bool", 1, 1);
        Row(1, "deposit_value", 1, 10);
        var config = (byte*)NativeMemory.AllocZeroed(0x20);
        *(uint*)(config + 0x10) = 2;
        *(byte**)(config + 0x18) = rows;
        var cell = (byte**)NativeMemory.AllocZeroed(8);
        *cell = config;

        // The entity region with the Maxigun backpack's deposit record at its vanilla values.
        var profile = EntityProfile.Current;
        var schema = profile.Components["DepositComponentData"];
        var region = VirtualAlloc(0, (nuint)profile.EntityRegionSize, 0x3000, 0x04);
        Expect(region != 0, "The synthetic entity region is allocated");
        Marshal.Copy(Convert.FromHexString(profile.MapHeader), 0, region, 28);
        var section = region + (nint)schema.Offset - 4;
        Marshal.WriteInt32(section, schema.Index);
        Marshal.Copy(Convert.FromHexString(schema.Header), 0, section + 4, 28);
        Marshal.WriteInt64(section + 32 + 24 * 16, unchecked((long)Backpack));
        Marshal.WriteInt32(section + 32 + 24 * 16 + 8, 23);
        var record = region + (nint)schema.Offset + 28 + schema.RecordOffset + 23 * schema.Stride;
        Marshal.WriteInt32(record, 1000);
        Marshal.WriteInt32(record + 4, 1000);
        Marshal.WriteInt32(record + 8, 500);
        // The game's entity data is read-only, as in the game: edits must make it writable.
        Expect(VirtualProtect(region, (nuint)profile.EntityRegionSize, 0x02, out _), "The synthetic entity region is made read-only");

        // The two mods and the plan.
        var hookFolder = Directory.CreateDirectory(Path.Combine(root, "mods", "hook-test")).FullName;
        File.Copy(Path.Combine(testDirectory, "hook_test_mod.dll"), Path.Combine(hookFolder, "hook_test_mod.dll"));
        // A second copy under another id hooks the same function: the two chain (SDK 13), each adding 1000.
        var hookFolder2 = Directory.CreateDirectory(Path.Combine(root, "mods", "hook-test-2")).FullName;
        File.Copy(Path.Combine(testDirectory, "hook_test_mod.dll"), Path.Combine(hookFolder2, "hook_test_mod.dll"));
        var gunFolder = Directory.CreateDirectory(Path.Combine(root, "mods", "machine-gun")).FullName;
        File.Copy(Path.Combine(machineGunMod, "machine_gun_10k.dll"), Path.Combine(gunFolder, "machine_gun_10k.dll"));
        var gameBase = (long)game;
        var plan = new StringBuilder()
            .Append("build\tsmoke\n").Append($"logs\t{root}\n")
            .Append($"game_pin\tfake_game.dll\t{(long)pin - gameBase:x}\t7575488b15fafc06024881c2e4030000\n")
            .Append($"network_config\tfake_game.dll\t{(long)holder - gameBase:x}\n")
            .Append($"entity_region\t{profile.EntityRegionSize}\t{profile.MapHeader}\n")
            .Append($"component\tDepositComponentData\t{schema.Offset}\t{schema.Header}\t{schema.Index}\t{schema.Indices}\t{schema.Records}\t{schema.RecordOffset}\t{schema.Stride}\n")
            .Append($"mod\ttest.hook\t{hookFolder}\t{Path.Combine(hookFolder, "hook_test_mod.dll")}\n")
            .Append("mod_version\ttest.hook\t9.9.9-smoke\n")
            // a DLL that is not there: modloader.log gives Windows' error
            .Append($"mod\ttest.missing\t{hookFolder}\t{Path.Combine(hookFolder, "missing.dll")}\n")
            .Append($"mod\ttest.hook2\t{hookFolder2}\t{Path.Combine(hookFolder2, "hook_test_mod.dll")}\n")
            .Append($"mod\tskyeshade.machine-gun-10k-code\t{gunFolder}\t{Path.Combine(gunFolder, "machine_gun_10k.dll")}\n");
        // The Stratagem Compendium: every definition, field and value it registers is accepted (no game data needed yet).
        if (compendiumMod != null)
        {
            var compendiumFolder = Directory.CreateDirectory(Path.Combine(root, "mods", "compendium")).FullName;
            File.Copy(Path.Combine(compendiumMod, "stratagem_compendium.dll"), Path.Combine(compendiumFolder, "stratagem_compendium.dll"));
            plan.Append($"mod\tskyeshade.stratagem-compendium\t{compendiumFolder}\t{Path.Combine(compendiumFolder, "stratagem_compendium.dll")}\n");
        }
        var planPath = Path.Combine(root, "modloader-plan.txt");
        File.WriteAllText(planPath, plan.ToString());

        // The real chain: loading the steam_api64 stand-in starts the loader, as in the game.
        File.Copy(Path.Combine(standIns, "hellforge_modloader.dll"), Path.Combine(root, "hellforge_modloader.dll"));
        File.Copy(Path.Combine(standIns, "steam_api64.dll"), Path.Combine(root, "steam_api64.dll"));
        Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER", Path.Combine(root, "hellforge_modloader.dll"));
        Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER_PLAN", planPath);
        // the session runner's two events for this "game" (SDK 16, the session's end), made before the loader starts
        using var endSignal = SessionEndSignal.Create(Environment.ProcessId)!;
        NativeLibrary.Load(Path.Combine(root, "steam_api64.dll"));

        string Log(string name) => File.Exists(Path.Combine(root, name)) ? ReadShared(Path.Combine(root, name)) : "";
        bool WaitFor(Func<bool> condition)
        {
            var clock = Stopwatch.StartNew();
            while (clock.ElapsedMilliseconds < 5000) { if (condition()) return true; Thread.Sleep(20); }
            return false;
        }
        Expect(WaitFor(() => Log("modloader.log").Contains("skyeshade.machine-gun-10k-code: loaded") && Log("modloader.log").Contains("test.hook: loaded")),
            "The loader loads both mods");
        Expect(WaitFor(() => Log("mod-test.hook.log").Contains("detour installed") && Log("mod-test.hook2.log").Contains("detour installed")),
            "Both hook-test mods hook the game once it is loaded");
        // code_matches (SDK 13): the first checks the function as it is, the second after the first hooked it.
        Expect(Log("mod-test.hook.log").Contains("code check passed (live code hooked: no)") &&
            Log("mod-test.hook2.log").Contains("code check passed (live code hooked: yes)"),
            "Each hook-test mod's check of the function passes, the second's through the first's hook");
        Expect(WaitFor(() => Log("modloader.log").Contains("shared hook fake_game.dll+0x") && Log("hooks.tsv").Contains("2 of 2\ttest.hook\t")),
            "The shared hook is logged, the mod higher on the Mods page (test.hook2, later in the plan) first: " + Log("hooks.tsv"));
        // The Stratagem Compendium's rounds and damage (more than the 3 new projectile types the list has room for in
        // place: the list moves); its entities need the game's component profile, which the smoke plan does not have, so
        // the only refusals allowed are those.
        if (compendiumMod != null)
        {
            Expect(WaitFor(() => Log("mod-skyeshade.stratagem-compendium.log").Contains("stratagem(s) refused")), "The Stratagem Compendium initializes");
            var lines = Log("mod-skyeshade.stratagem-compendium.log").Split('\n');
            int Count(string text) => lines.Count(l => l.Contains(text, StringComparison.Ordinal));
            Expect(Count("adding projectile") == 9 && Count("adding damage") == 9, "The Stratagem Compendium adds its 9 rounds and 9 damage types");
            Expect(Count("not added (see above)") <= Count("does not know that component") &&
                !lines.Any(l => l.Contains("add_projectile(", StringComparison.Ordinal) || l.Contains("add_damage(", StringComparison.Ordinal) ||
                    l.Contains("line ", StringComparison.Ordinal)),
                "The Stratagem Compendium's only refusals are the smoke plan's missing components: " + Log("mod-skyeshade.stratagem-compendium.log"));
        }
        Expect(Log("mod-test.hook.log").Contains("pattern found the pin"), "find_pattern finds the pin");
        var hookHash = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(Path.Combine(hookFolder, "hook_test_mod.dll"))));
        Expect(Log("modloader.log").Contains($"test.hook: version 9.9.9-smoke, hook_test_mod.dll, SHA-256 {hookHash}"),
            "modloader.log gives a code mod's version and its DLL's SHA-256");
        Expect(Log("modloader.log").Contains("test.hook2: version not given, hook_test_mod.dll, SHA-256 " + hookHash), "A mod without a version line says so");
        Expect(Log("modloader.log").Contains("test.missing: not loaded (the DLL could not be loaded: error 126: "), "A DLL that does not load gives its error");
        Expect(CloudSmoke.OpensDated(Path.Combine(root, "modloader.log")), "modloader.log opens with its dated line");
        Expect(CloudSmoke.OpensDated(Path.Combine(root, "mod-test.hook.log")), "A mod's log opens with its dated line");
        Expect(target(5) == 2016, "Both detours run, each calling the next, then the original");
        Expect(WaitFor(() => Marshal.ReadInt32(record) == 10000 && Marshal.ReadInt32(record + 4) == 10000 && Marshal.ReadInt32(record + 8) == 5000),
            "The Machine Gun mod fills the backpack's deposit record once the entity data is ready");

        // Live editing: the launcher's pipe, the loader as its client, a stats field changed by identity and undone.
        var capacity = StatsSchema.Current.Targets["backpacks"]["M-1000 Maxigun Backpack"].Fields["deposit.capacity"];
        var live = new LiveEditServer(Path.GetFileName(root), build: "smoke");
        live.Start();
        Expect(WaitFor(() => live.Connected), "The loader connects to the launcher's live pipe once the entity data is ready");
        var set = live.SetAsync(capacity, 12345, shared: false).Result;
        Expect(set is { Ok: true, Detail: "respawn" } && Marshal.ReadInt32(record) == 12345, $"A live change reaches the backpack's record ({set.Detail})");
        Expect(live.GetAsync(capacity).Result.Bits == 12345, "A live get reads the game's value");
        var reset = live.ResetAsync(capacity).Result;
        Expect(reset.Ok && Marshal.ReadInt32(record) == 10000, "A live reset puts back the value from before it");
        Expect(Log("modloader.log").Contains("conflict: skyeshade.machine-gun-10k-code and zz.local.stats"), "The live edit's conflict with a mod's edit is reported");
        live.DisposeAsync().AsTask().Wait();
        Thread.Sleep(200);
        Expect(rows[0x18 + 13] == 10 && !Log("modloader.log").Contains("network configuration ready"), "Nothing reads the network configuration before it is loaded");
        *(byte***)holder = cell;
        Expect(WaitFor(() => rows[0x18 + 13] == 14), "The Machine Gun mod widens deposit_value once the network configuration is ready");
        Expect(rows[13] == 1, "Other field types are untouched");
        Expect(Log("mod-skyeshade.machine-gun-10k-code.log").Contains("holds up to 16383"), "The mod logs to its own log");
        SessionEndChecks(root, endSignal, Expect);

        Console.WriteLine($"Mod loader smoke: {(problems.Count == 0 ? "passed" : problems.Count + " problem(s)")}; logs in {root}");
        foreach (var problem in problems) Console.Error.WriteLine("  failed: " + problem);
        return problems.Count == 0 ? 0 : 1;
    }

    // SDK 16: the hook-test mods' status in mod-status.tsv, then the session's end as the session runner tells it: the mods
    // hear it, the loader answers, and their status says so.
    static void SessionEndChecks(string root, SessionEndSignal endSignal, Action<bool, string> expect)
    {
        string Log(string name) => File.Exists(Path.Combine(root, name)) ? ReadShared(Path.Combine(root, name)) : "";
        var statuses = ModStatus.Read(root);
        expect(statuses?.GetValueOrDefault("test.hook") == "detour installed", "set_status reaches mod-status.tsv, its tab a space: " + Log(ModStatus.FileName));
        expect(statuses?.GetValueOrDefault("test.hook2") == "detour installed", "Each mod has its own status");
        expect(Log("mod-test.hook.log").Contains("status: detour installed"), "The mod's log records its status");
        expect(endSignal.Signal(TimeSpan.FromSeconds(5)), "The loader answers the session's end");
        expect(Log("modloader.log").Contains("session end: the session is stopping"), "modloader.log notes the session's end");
        expect(Log("mod-test.hook.log").Contains("heard the session end") && Log("mod-test.hook2.log").Contains("heard the session end"),
            "Both hook-test mods hear the session's end");
        expect(ModStatus.Read(root)?.GetValueOrDefault("test.hook") == "session over", "A status set at the session's end is kept");
    }

    static string ReadShared(string path)
    {
        using var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
        return reader.ReadToEnd();
    }

    [DllImport("kernel32.dll", SetLastError = true)] static extern nint VirtualAlloc(nint address, nuint size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool VirtualProtect(nint address, nuint size, uint protect, out uint previous);
}
