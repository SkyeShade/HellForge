using System.Text.Json.Nodes;
using HellForge.Core;

// The session's mods/ hold (lockdown item 4, ModsHold) and the sandboxed editor's saves applied after the session
// (SandboxStats).
static class ModsHoldTests
{
    static string Manifest(string id, string name) =>
        $$$"""{"format":1,"id":"{{{id}}}","name":"{{{name}}}","version":"1.0","native":{"networkFieldTypes":[{"name":"{{{id.Replace('-', '_')}}}","expect":{"kind":1,"bits":8,"min":0},"bits":9}]}}""";

    const string Settings = """{"format":1,"disabled":[],"order":[],"packs":{},"blocked":[],"share":true,"showEditedStats":false}""";

    static bool Refused(Action write)
    {
        try { write(); return false; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return true; }
    }

    // A mods folder with a mod, the settings, the editor and Local Stats, as a session finds it.
    static string Folder(string root, string name)
    {
        var mods = Directory.CreateDirectory(Path.Combine(root, name, "mods")).FullName;
        var mod = Directory.CreateDirectory(Path.Combine(mods, "test-mod")).FullName;
        File.WriteAllText(Path.Combine(mod, ModCatalog.ManifestName), Manifest("test-mod", "Test"));
        File.WriteAllText(Path.Combine(mods, ModSettings.FileName), Settings);
        var editor = Directory.CreateDirectory(Path.Combine(mods, BundledMods.EditorFolder, "presets")).Parent!.FullName;
        File.WriteAllText(Path.Combine(editor, "hellforge_editor.dll"), "the editor");
        File.WriteAllText(Path.Combine(editor, "editor-settings.json"), "{}");
        File.WriteAllText(Path.Combine(mods, ".hellforge-stats-editor.lock"), "");
        return mods;
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Mods hold: mod files are held read-only; the files HellForge writes during a session are not", () =>
        {
            var mods = Folder(root, "mods-hold");
            var editor = Path.Combine(mods, BundledMods.EditorFolder);
            using var hold = ModsHold.Take(mods);
            check(hold.Held == 2, $"The manifest and the editor's DLL are held ({hold.Held}).");
            check(Refused(() => File.WriteAllText(Path.Combine(mods, "test-mod", ModCatalog.ManifestName), "replaced")), "A mod's manifest cannot be overwritten.");
            check(Refused(() => File.Move(Path.Combine(editor, "hellforge_editor.dll"), Path.Combine(editor, "moved.dll"))), "The editor's DLL cannot be renamed.");
            // as ModSettings, the in-game editor (WriteAtomically: a temporary file moved over) and the launcher write them
            File.WriteAllText(Path.Combine(mods, ModSettings.FileName + ".tmp"), Settings);
            File.Move(Path.Combine(mods, ModSettings.FileName + ".tmp"), Path.Combine(mods, ModSettings.FileName), overwrite: true);
            File.WriteAllText(Path.Combine(editor, "editor-settings.json.ingame.tmp"), "{\"ui_scale\":1}");
            File.Move(Path.Combine(editor, "editor-settings.json.ingame.tmp"), Path.Combine(editor, "editor-settings.json"), overwrite: true);
            using (new FileStream(Path.Combine(mods, ".hellforge-stats-editor.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) { }
            File.WriteAllText(Path.Combine(editor, "presets", "mine.hd2rpreset.json"), "{}");
            check(true, "The settings, the editor's files and the stats lock stay writable.");
            hold.Release();
            File.WriteAllText(Path.Combine(mods, "test-mod", ModCatalog.ManifestName), Manifest("test-mod", "Test"));
            check(true, "Released, the mod can be written again.");
        });

        test("Mods hold: what appeared is reported and new mod folders switched off; HellForge's own files are not", () =>
        {
            var mods = Folder(root, "mods-planted");
            var hold = ModsHold.Take(mods);
            // the game's
            var planted = Directory.CreateDirectory(Path.Combine(mods, "planted-mod")).FullName;
            File.WriteAllText(Path.Combine(planted, ModCatalog.ManifestName), Manifest("planted-mod", "Planted"));
            File.WriteAllText(Path.Combine(planted, "payload.dll"), "not a real DLL");
            File.WriteAllText(Path.Combine(mods, "test-mod", "extra.dll"), "snuck in");
            // HellForge's: the editor's first save makes Local Stats, a preset, the host's files, a mod sync download
            var stats = Path.Combine(mods, ModCatalog.LocalStatsId);
            StatsEditorData.Save(mods, new JsonObject { ["format"] = 1, ["weapons"] = new JsonObject { ["AR-23 Liberator"] = new JsonObject { ["weapon.fire_rate"] = 900 } } }, "missing");
            File.WriteAllText(Path.Combine(mods, BundledMods.EditorFolder, "presets", "new.hd2rpreset.json"), "{}");
            File.WriteAllText(Path.Combine(mods, BundledMods.EditorFolder, "host-stats.json"), "{}");
            Directory.CreateDirectory(Path.Combine(SyncedStore.Root(mods), "someone.mod@0123456789abcdef"));
            hold.Release();
            var found = hold.Check(_ => { });
            check(found != null, "Planted files are reported.");
            check(found!.Files.Contains("planted-mod/") && found.Files.Contains("planted-mod/payload.dll") && found.Files.Contains("test-mod/extra.dll"),
                "The planted folder and the file planted in an existing mod are listed: " + string.Join(", ", found.Files));
            check(found.Files.All(f => !f.StartsWith(ModCatalog.LocalStatsId) && !f.StartsWith(BundledMods.EditorFolder) && !f.StartsWith(SyncedStore.FolderName)),
                "HellForge's own files are not reported.");
            check(found.DisabledMods.Order().SequenceEqual(["planted-mod", "test-mod"]) && ModSettings.Disabled(mods).Contains("planted-mod") &&
                !ModSettings.Disabled(mods).Contains(ModCatalog.LocalStatsId) && !ModSettings.Disabled(mods).Contains("hellforge.editor"),
                "The planted mod and the mod a file was added to are switched off; HellForge's are not: " + string.Join(", ", found.DisabledMods));
            check(Directory.Exists(stats), "Local Stats stays.");
        });

        test("Mods hold: a Local Stats folder that is not the editor's is reported", () =>
        {
            var mods = Folder(root, "mods-fake-stats");
            var hold = ModsHold.Take(mods);
            var stats = Directory.CreateDirectory(Path.Combine(mods, ModCatalog.LocalStatsId)).FullName;
            File.WriteAllText(Path.Combine(stats, ModCatalog.ManifestName),
                $$$"""{"format":1,"id":"{{{ModCatalog.LocalStatsId}}}","name":"Local Stats","version":"1.0.0","gameplay":true,"editor":"{{{StatsEditorData.Marker}}}","code":{"dll":"x.dll","sdk":6}}""");
            File.WriteAllText(Path.Combine(stats, "x.dll"), "code");
            hold.Release();
            var found = hold.Check(_ => { });
            check(found != null && found.Files.Contains(ModCatalog.LocalStatsId + "/") && found.Files.Contains(ModCatalog.LocalStatsId + "/x.dll"),
                "A Local Stats folder with code in it is reported.");
        });

        test("Mods hold: an unchanged folder reports nothing, and a held file elsewhere stops the hold", () =>
        {
            var mods = Folder(root, "mods-unchanged");
            using (var hold = ModsHold.Take(mods)) { hold.Release(); check(hold.Check(_ => { }) == null, "Nothing appeared, nothing reported."); }
            using var other = new FileStream(Path.Combine(mods, "test-mod", ModCatalog.ManifestName), FileMode.Open, FileAccess.ReadWrite, FileShare.None);
            reject(() => ModsHold.Take(mods).Dispose());
        });

        test("Sandboxed sessions: the game's data is copied in and back, files only, never through a junction, and bounded", () =>
        {
            var install = Directory.CreateDirectory(Path.Combine(root, "sandbox-copy", "userdata")).FullName;
            var session = Path.Combine(root, "sandbox-copy", "run", "game", "userdata");
            Directory.CreateDirectory(Path.Combine(install, "profiles", "default", "saves"));
            File.WriteAllText(Path.Combine(install, "profiles", "default", "saves", "save.dat"), "save");
            Directory.CreateDirectory(Path.Combine(install, "profiles", "other"));
            File.WriteAllText(Path.Combine(install, "profiles", "other", "x.dat"), "other profile");
            Directory.CreateDirectory(Path.Combine(install, "shader_cache"));
            File.WriteAllText(Path.Combine(install, "shader_cache", "cache.bin"), new string('c', 1000));
            File.WriteAllText(Path.Combine(install, "dearimgui.ini"), "[ImGui]");
            Directory.CreateDirectory(Path.Combine(install, "logs"));
            File.WriteAllText(Path.Combine(install, "logs", "old.txt"), "log");
            var copied = SandboxFolders.Copy(install, session, SandboxFolders.UserDataParts("default"));
            check(copied is { Files: 3, Skipped: 0 } && File.Exists(Path.Combine(session, "profiles", "default", "saves", "save.dat")) &&
                !Directory.Exists(Path.Combine(session, "profiles", "other")) && !Directory.Exists(Path.Combine(session, "logs")),
                "In: the profile, the shader cache and the ImGui settings; not other profiles or logs: " + copied);

            // the game wrote a save, and a junction to a folder outside (a low process can make one in its own folder)
            File.WriteAllText(Path.Combine(session, "profiles", "default", "saves", "save.dat"), "save 2");
            var outside = Directory.CreateDirectory(Path.Combine(root, "sandbox-copy", "outside")).FullName;
            File.WriteAllText(Path.Combine(outside, "secret.txt"), "not the game's");
            var junction = Path.Combine(session, "profiles", "default", "link");
            using (var made = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/d /c mklink /J \"{junction}\" \"{outside}\"")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!)
                made.WaitForExit();
            check(Directory.Exists(junction), "The junction was made.");
            var back = SandboxFolders.Copy(session, install, SandboxFolders.UserDataParts("default"));
            check(File.ReadAllText(Path.Combine(install, "profiles", "default", "saves", "save.dat")) == "save 2" && back.Skipped == 1 &&
                !File.Exists(Path.Combine(install, "profiles", "default", "link", "secret.txt")), "Back: the new save; the junction is not followed: " + back);
            check(SandboxFolders.Copy(session, install, SandboxFolders.UserDataParts("default"), maxBytes: 10) is { Skipped: > 0 }, "A copy stops at its byte limit.");
            Directory.Delete(junction);
        });

        test("Sandboxed editor saves: changes are applied onto the files as they are now, each checked", () =>
        {
            var mods = Folder(root, "sandbox-stats");
            var sandbox = Directory.CreateDirectory(Path.Combine(root, "sandbox-stats", "game", "sandbox-stats")).FullName;
            var revision = StatsEditorData.Save(mods, new JsonObject { ["format"] = 1, ["weapons"] = new JsonObject { ["AR-23 Liberator"] = new JsonObject { ["magazine.capacity"] = 45 } } }, "missing");
            File.WriteAllText(Path.Combine(sandbox, SandboxStats.ChangesName), """
                {"format":1,"changes":[
                  {"category":"weapons","target":"AR-23 Liberator","key":"weapon.fire_rate","value":900},
                  {"category":"weapons","target":"AR-23 Liberator","key":"weapon.not_a_field","value":1},
                  {"category":"weapons","target":"AR-23 Liberator","key":"weapon.recoil","value":"x"},
                  {"category":"mod:you/mod/sentry","target":"you/mod/sentry","key":"stratagem.cooldown","value":150},
                  {"category":"format","target":"x","key":"y","value":1}
                ]}
                """);
            File.WriteAllText(Path.Combine(sandbox, SandboxStats.SettingsName), """{"ui_scale":1.25}""");
            Directory.CreateDirectory(Path.Combine(sandbox, "presets"));
            File.WriteAllText(Path.Combine(sandbox, "presets", "Mine.hd2rpreset.json"), """{"format":"hd2r-editor-preset","version":1,"fields":[]}""");
            File.WriteAllText(Path.Combine(sandbox, "presets", "..evil.dll"), "code");
            var applied = SandboxStats.Apply(sandbox, mods, EntityProfile.Current);
            check(applied is { Stats: 1, ModValues: 1, Refused: 3, Settings: true, Presets: 1 }, "Applied: " + applied);
            var (document, _) = StatsEditorData.Load(mods);
            check((int?)document["weapons"]?["AR-23 Liberator"]?["weapon.fire_rate"] == 900 && (int?)document["weapons"]?["AR-23 Liberator"]?["magazine.capacity"] == 45 &&
                document["weapons"]?["AR-23 Liberator"]?["weapon.not_a_field"] == null, "The valid change joins the file as it was; the refused ones do not.");
            var editor = Path.Combine(mods, BundledMods.EditorFolder);
            check(File.ReadAllText(Path.Combine(editor, SandboxStats.ModValuesName)).Contains("\"stratagem.cooldown\": 150"), "Mod content values go to the editor's file.");
            check(File.ReadAllText(Path.Combine(editor, SandboxStats.SettingsName)).Contains("1.25"), "The editor's settings are put in place.");
            check(File.Exists(Path.Combine(editor, "presets", "Mine.hd2rpreset.json")) && !Directory.EnumerateFiles(editor, "*.dll", SearchOption.AllDirectories).Any(f => f.Contains("evil")),
                "Presets with a plain name are copied; nothing else is.");

            // removing the last change takes Local Stats away, as the STATS page does
            File.WriteAllText(Path.Combine(sandbox, SandboxStats.ChangesName), """
                {"format":1,"changes":[{"category":"weapons","target":"AR-23 Liberator","key":"weapon.fire_rate","remove":true},
                                       {"category":"weapons","target":"AR-23 Liberator","key":"magazine.capacity","remove":true}]}
                """);
            File.Delete(Path.Combine(sandbox, SandboxStats.SettingsName));
            Directory.Delete(Path.Combine(sandbox, "presets"), recursive: true);
            check(SandboxStats.Apply(sandbox, mods, EntityProfile.Current) is { Stats: 2, Refused: 0 } && !Directory.Exists(Path.Combine(mods, ModCatalog.LocalStatsId)),
                "Removing every change removes Local Stats.");
            File.WriteAllText(Path.Combine(sandbox, SandboxStats.ChangesName), "{\"format\":2,\"changes\":[]}");
            reject(() => SandboxStats.Apply(sandbox, mods, EntityProfile.Current));
            File.Delete(Path.Combine(sandbox, SandboxStats.ChangesName));
            check(SandboxStats.Apply(sandbox, mods, EntityProfile.Current) == null, "Nothing saved, nothing applied.");
        });
    }
}
