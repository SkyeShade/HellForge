using HellForge.Core;
using HellForge.Core.GameData;

// Changing a running session's mods (LiveModPlan): stats mods added, removed and replaced plan live, with the loader's
// lines; so do own records, network field types (modnet, the boot plan's nettype fields), code that declares "live" and
// data patches (after mods-live: its hot package's lines); code without "live" and a new order wait for the next session;
// a mod that did not load at the start is not new; the session's mods file round-trips. Every mod here is synthetic.
static class LiveModsTests
{
    const string Rpm = """{"format":1,"weapons":{"AR-23 Liberator":{"rpm":900}}}""";

    static void Mod(string mods, string id, string extra = "", string? stats = null)
    {
        var folder = Path.Combine(mods, id);
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), $$"""{"format":1,"id":"{{id}}","name":"{{id}} mod","version":"1"{{extra}}}""");
        if (stats != null) File.WriteAllText(Path.Combine(folder, StatsData.FileName), stats);
    }

    static List<ModManifest> Session(string mods) => ModCatalog.LoadOrder(ModCatalog.Active(ModCatalog.Load(mods).Mods));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Live mod changes: stats, own records, network types, live code and data plan live with the loader's lines; other code and a new order wait", () =>
        {
            var mods = Path.Combine(root, "live-mods");
            Mod(mods, "s1", stats: Rpm);
            Mod(mods, "s2", stats: Rpm.Replace("900", "800"));
            Mod(mods, "c1", ",\"code\":{\"dll\":\"mod.dll\",\"sdk\":1}");
            File.WriteAllText(Path.Combine(mods, "c1", "mod.dll"), "synthetic dll");
            Mod(mods, "v1", ",\"gameplay\":false");
            var (archive, _, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(ResourceTypes.Texture, 0x1234)] = new([1, 2, 3, 4], [], []) });
            Directory.CreateDirectory(Path.Combine(mods, "v1", "data"));
            File.WriteAllBytes(Path.Combine(mods, "v1", "data", "9ba626afa44a3aa3.patch_0"), archive);
            var started = Session(mods);
            var current = SessionMods.Of(started, started, "");
            check(LiveModPlan.Make(current, Session(mods)).Empty, "An unchanged session planned a change.");

            // s1's stats changed: replaced live; its modstat fields are the boot plan's stat fields
            File.WriteAllText(Path.Combine(mods, "s1", StatsData.FileName), Rpm.Replace("900", "1000"));
            var plan = LiveModPlan.Make(current, Session(mods));
            check(plan.Live && plan.Changes.Single() is { Id: "s1", Action: LiveModAction.Replace }, "A changed stats mod did not plan a live replace.");
            var lines = plan.Lines("t1", sync: false, "digest").ToList();
            var s1 = plan.Target.Single(m => m.Id == "s1");
            var bootStat = "stat\t" + string.Join('\t', ModLoaderPlan.StatFields(s1, StatsData.Edits(s1).Single()));
            check(ModLoaderPlan.Text(plan.Target, root, EntityProfile.Current, "25480438").Contains(bootStat + "\n"), "The boot plan's stat line differs.");
            check(lines[0].Verb == "modbegin" && lines[0].Fields.SequenceEqual(["t1", "solo", "digest"]) &&
                lines.Where(l => l.Verb == "modorder").Select(l => l.Fields[2]).SequenceEqual(plan.Target.Where(m => m.NeedsLoader).Select(m => m.Id)) &&
                lines.Any(l => l.Verb == "modreplace" && l.Fields[1] == "s1" && l.Fields[2] == "stats" && l.Fields[5] == "-") &&
                lines.Single(l => l.Verb == "modstat").Fields.Skip(1).SequenceEqual(bootStat.Split('\t').Skip(1)) && lines[^1].Verb == "modcommit",
                "The loader's lines are wrong: " + string.Join(" | ", lines.Select(l => l.Verb + " " + string.Join(',', l.Fields))));
            File.WriteAllText(Path.Combine(mods, "s1", StatsData.FileName), Rpm);

            // s2 off and a new stats mod on: removed and added live
            ModSettings.SetEnabled(mods, "s2", false);
            Mod(mods, "s3", stats: Rpm.Replace("900", "700"));
            plan = LiveModPlan.Make(current, Session(mods));
            check(plan.Live && plan.Changes.Any(c => c is { Id: "s2", Action: LiveModAction.Remove }) && plan.Changes.Any(c => c is { Id: "s3", Action: LiveModAction.Add }),
                "Off and new stats mods did not plan live.");
            var verbs = plan.Lines("t2", sync: true, "digest").ToList();
            check(verbs.Any(l => l.Verb == "modremove" && l.Fields[1] == "s2") && verbs.Any(l => l.Verb == "modadd" && l.Fields[1] == "s3") && verbs[0].Fields[1] == "sync",
                "Remove and add lines are wrong.");
            ModSettings.SetEnabled(mods, "s2", true);
            Directory.Delete(Path.Combine(mods, "s3"), true);

            // code without "live" waits for the next session
            ModSettings.SetEnabled(mods, "c1", false);
            plan = LiveModPlan.Make(current, Session(mods));
            check(!plan.Live && plan.Blocking.Any(b => b.StartsWith("c1", StringComparison.Ordinal) && b.Contains("live")), "A code mod switched off planned live.");
            ModSettings.SetEnabled(mods, "c1", true);
            // a changed data patch plans live once mods-live has made its hot package: the package's lines go last
            File.WriteAllBytes(Path.Combine(mods, "v1", "data", "9ba626afa44a3aa3.patch_0"), [.. archive, 0]);
            plan = LiveModPlan.Make(current, Session(mods));
            check(plan.Live && plan.NeedsPreparation, "A changed data patch did not plan live.");
            reject(() => plan.Lines("t3", sync: false, "digest").ToList());
            var prepared = new LivePrepared(new Dictionary<string, string>(), [["moddata", "-", "0123456789abcdef"]]);
            var data = plan.Lines("t3", sync: false, "digest", prepared).ToList();
            check(data[^2].Verb == "moddata" && data[^2].Fields.SequenceEqual(["t3", "-", "0123456789abcdef"]) && !data.Any(l => l.Verb == "modreplace"),
                "A data mod's change is not its package's line.");
            File.WriteAllBytes(Path.Combine(mods, "v1", "data", "9ba626afa44a3aa3.patch_0"), archive);
            // own records plan live (the loader copies the record)
            Mod(mods, "o1", stats: """{"format":1,"weapons":{"AR-23 Liberator":{"own":true,"rpm":950}}}""");
            plan = LiveModPlan.Make(current, Session(mods));
            check(plan.Live && plan.Lines("t4", sync: false, "digest").Single(l => l.Verb == "modstat").Fields[10] == "1", "Own records did not plan live: " + string.Join(" | ", plan.Blocking));
            Directory.Delete(Path.Combine(mods, "o1"), true);
            // network field types: the mod is the loader's, its modnet lines are the boot plan's nettype fields
            Mod(mods, "n1", ",\"native\":{\"networkFieldTypes\":[{\"name\":\"deposit_amount\",\"bits\":12,\"expect\":{\"kind\":1,\"bits\":8,\"min\":0}}]}");
            plan = LiveModPlan.Make(current, Session(mods));
            var n1 = plan.Target.Single(m => m.Id == "n1");
            var net = plan.Lines("t5", sync: false, "digest").ToList();
            check(plan.Live && n1.NeedsLoader && net.Any(l => l.Verb == "modadd" && l.Fields[1] == "n1" && l.Fields[2] == "net") &&
                net.Single(l => l.Verb == "modnet").Fields.Skip(1).SequenceEqual(ModLoaderPlan.NetFields(n1, n1.NetworkFieldTypes.Single())) &&
                ModLoaderPlan.Text(plan.Target, root, EntityProfile.Current, "25480438").Contains("nettype\t" + string.Join('\t', ModLoaderPlan.NetFields(n1, n1.NetworkFieldTypes.Single())) + "\n"),
                "Network field types did not plan live with their lines.");
            Directory.Delete(Path.Combine(mods, "n1"), true);
            // code that declares "live" (SDK 16): added live, its DLL on its line, "live" on its boot plan line
            Mod(mods, "l1", ",\"code\":{\"dll\":\"mod.dll\",\"sdk\":16,\"live\":true}");
            File.WriteAllText(Path.Combine(mods, "l1", "mod.dll"), "synthetic live dll");
            plan = LiveModPlan.Make(current, Session(mods));
            var l1 = plan.Target.Single(m => m.Id == "l1");
            check(plan.Live && l1.Live && plan.Lines("t6", sync: false, "digest").Any(l => l.Verb == "modadd" && l.Fields[1] == "l1" && l.Fields[2] == "code,live" &&
                l.Fields[5] == l1.DllPath) && ModLoaderPlan.Text(plan.Target, root, EntityProfile.Current, "25480438").Contains($"mod\tl1\t{l1.Folder}\t{l1.DllPath}\tlive\n"),
                "A live code mod did not plan live.");
            Directory.Delete(Path.Combine(mods, "l1"), true);
            var order = ModCatalog.Load(mods).Mods.Select(m => m.Id).ToList();
            ModSettings.SetOrder(mods, order.AsEnumerable().Reverse());
            plan = LiveModPlan.Make(current, Session(mods));
            check(!plan.Live && plan.Blocking.Contains("The mods' order changed"), "A new order planned live.");
            ModSettings.SetOrder(mods, order);
            check(LiveModPlan.Make(current, Session(mods)).Empty, "Restoring everything left a change.");

            // a mod that did not load at the start (a source mod that did not build) is not new while it is unchanged
            var loaded = started.Where(m => m.Id != "c1").ToList();
            var withoutCode = SessionMods.Of(started, loaded, "");
            check(LiveModPlan.Make(withoutCode, Session(mods)).Empty && withoutCode.Skipped.Single().Id == "c1", "A mod that did not load counted as added.");
            File.WriteAllText(Path.Combine(mods, "c1", "mod.dll"), "another synthetic dll");
            check(!LiveModPlan.Make(withoutCode, Session(mods)).Live, "A changed mod that did not load planned live.");

            // the session's mods file round-trips
            var file = Path.Combine(root, "live-mods-session.json");
            withoutCode.Write(file);
            var read = SessionMods.Read(file);
            check(read.Mods.SequenceEqual(withoutCode.Mods) && read.Skipped.SequenceEqual(withoutCode.Skipped) && read.SyncedWith == "", "The session's mods did not round-trip.");
            File.WriteAllText(file, File.ReadAllText(file).Replace(read.Mods[0].Hash, "nothex"));
            reject(() => SessionMods.Read(file));
        });

        test("Live mod changes: a change's target is read back from its folders, a cosmetic pack and a host's downloaded mod among them", () =>
        {
            var mods = Path.Combine(root, "live-target");
            Mod(mods, "s1", stats: Rpm);
            var pack = Path.Combine(mods, "a-pack");
            Directory.CreateDirectory(pack);
            var (archive, _, _) = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts> { [new(ResourceTypes.Texture, 0x1234)] = new([1, 2, 3, 4], [], []) });
            File.WriteAllBytes(Path.Combine(pack, "9ba626afa44a3aa3.patch_0"), archive);
            Mod(Path.Combine(mods, SyncedStore.FolderName), "h1", stats: Rpm.Replace("900", "800"));
            var session = Session(mods);
            var folders = session.Select(m => m.Folder).Append(Path.Combine(mods, SyncedStore.FolderName, "h1")).Append("").ToList();
            var target = LiveModPlan.TargetOf(mods, folders);
            check(target.Select(m => m.Id).SequenceEqual(session.Select(m => m.Id).Append("h1")) && target.Any(m => m.Pack != null) &&
                target.Single(m => m.Id == "h1").Stats.Count == 1, "The target's folders did not read back: " + string.Join(", ", target.Select(m => m.Id)));
            reject(() => LiveModPlan.TargetOf(mods, [Path.Combine(mods, "not-a-mod")]));
        });
    }
}
