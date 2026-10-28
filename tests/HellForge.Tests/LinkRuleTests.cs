using System.Security.Cryptography;
using HellForge.Core;

// The one link rule: a link in a folder above the folder HellForge protects is normal on Linux (/home -> /var/home on
// Fedora Atomic and Bazzite) and is resolved; a link at or inside the protected folder is still refused. Windows keeps
// its old rule, where every parent is checked.
static class LinkRuleTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Linux accepts an installation, game folder and Steam library reached through a linked parent and resolves them", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux link rule)"); return; }
            var (_, linkedHome, realHome) = Home(root, "linux-link-parents");

            var linkedInstall = Path.Combine(linkedHome, "HellForge");
            var realInstall = Path.Combine(realHome, "HellForge");
            var install = new InstallLayout(linkedInstall);
            install.Create("12345");
            check(install.Root == realInstall, "The installation was not resolved: " + install.Root);
            check(Directory.Exists(Path.Combine(realInstall, "game")),
                "The installation was not written at the resolved folder.");
            var inside = Path.Combine(realInstall, "game", "asset.bin");
            File.WriteAllText(inside, "asset");
            check(LocalInstall.NoLinks(Path.Combine(linkedInstall, "game", "asset.bin"), linkedInstall) == inside,
                "A path inside the installation was not resolved.");

            var realSteam = Path.Combine(realHome, "Steam");
            var source = Path.Combine(realSteam, "steamapps", "common", "Helldivers 2");
            Directory.CreateDirectory(Path.Combine(source, "bin"));
            Directory.CreateDirectory(Path.Combine(source, "data", "game"));
            File.WriteAllText(Path.Combine(source, LocalInstall.Exe), "synthetic executable");
            File.WriteAllText(Path.Combine(source, LocalInstall.GameDll), "synthetic game library");
            File.WriteAllText(Path.Combine(source, "data", "asset"), "synthetic asset");
            var manifest = Path.Combine(realSteam, "steamapps", "appmanifest_553850.acf");
            File.WriteAllText(manifest, """
                "AppState" { "appid" "553850" "StateFlags" "4" "installdir" "Helldivers 2" "buildid" "12345" }
                """);
            var other = Path.Combine(realHome, "OtherLibrary");
            Directory.CreateDirectory(Path.Combine(other, "steamapps"));
            File.WriteAllText(Path.Combine(realSteam, "steamapps", "libraryfolders.vdf"),
                "\"libraryfolders\" { \"0\" { \"path\" \"" + Path.Combine(linkedHome, "OtherLibrary") + "\" } }");
            var libraries = LocalInstall.FindLibraries(Path.Combine(linkedHome, "Steam"));
            check(libraries.Contains(realSteam) && libraries.Contains(other),
                "A Steam library reached through a linked parent was not resolved: " + string.Join(", ", libraries));
            var report = LocalInstall.Inspect(Path.Combine(linkedHome, "Steam", "steamapps", "appmanifest_553850.acf"));
            check(report.Source == source && report.ManifestPath == manifest,
                "Inspecting Steam's game through a linked parent did not use the resolved paths.");
            var prepared = CopyPreparer.Prepare(Path.Combine(linkedHome, "Steam", "steamapps", "appmanifest_553850.acf"),
                Path.Combine(linkedHome, "prepared"), report.Pin);
            check(prepared.Source.Source == source && File.Exists(Path.Combine(realHome, "prepared", "game", LocalInstall.Exe)),
                "Preparing a research copy through a linked parent did not use the resolved paths.");

            var copySource = Path.Combine(realHome, "copy", "source");
            var copyDestination = Path.Combine(realHome, "copy", "game");
            Directory.CreateDirectory(Path.Combine(copySource, "data"));
            File.WriteAllText(Path.Combine(copySource, "data", "asset"), "copied asset");
            var build = Build("data/asset", "copied asset");
            GameCopier.Copy(Path.Combine(linkedHome, "copy", "source"), Path.Combine(linkedHome, "copy", "game"), build);
            check(File.ReadAllText(Path.Combine(copyDestination, "data", "asset")) == "copied asset",
                "The game folder reached through a linked parent was not copied at the resolved path.");
        });

        test("Linux refuses a link at the installation, inside its game folder, or in a copy source", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux link rule)"); return; }
            var (sandbox, linkedHome, realHome) = Home(root, "linux-link-inside");
            var outside = Directory.CreateDirectory(Path.Combine(sandbox, "outside")).FullName;

            // The installation folder itself is a link: writing an installation through it would write somewhere else.
            var installTarget = Directory.CreateDirectory(Path.Combine(realHome, "install-target")).FullName;
            var linkedInstall = Path.Combine(linkedHome, "linked-install");
            Directory.CreateSymbolicLink(linkedInstall, installTarget);
            reject(() => new InstallLayout(linkedInstall).Create("12345"));

            // A link inside an installation is refused on the way to the file it would read or write.
            var realInstall = Path.Combine(realHome, "HellForge");
            Directory.CreateDirectory(realInstall);
            Directory.CreateSymbolicLink(Path.Combine(realInstall, "linked"), outside);
            reject(() => LocalInstall.NoLinks(Path.Combine(linkedHome, "HellForge", "linked", "file"), Path.Combine(linkedHome, "HellForge")));

            // A link inside a copy source is refused before its file is read.
            var source = Path.Combine(realHome, "copy-source");
            Directory.CreateDirectory(Path.Combine(source, "data"));
            Directory.CreateSymbolicLink(Path.Combine(source, "linked"), outside);
            File.WriteAllText(Path.Combine(outside, "asset"), "linked asset");
            reject(() => GameCopier.Copy(source, Path.Combine(realHome, "copy-destination"), Build("linked/asset", "linked asset")));

            // A link inside the game folder is refused before its file is written through.
            var gameSource = Path.Combine(realHome, "game-source");
            Directory.CreateDirectory(Path.Combine(gameSource, "data"));
            File.WriteAllText(Path.Combine(gameSource, "data", "asset"), "copied asset");
            var gameDestination = Directory.CreateDirectory(Path.Combine(realHome, "game-destination")).FullName;
            Directory.CreateSymbolicLink(Path.Combine(gameDestination, "data"), outside);
            reject(() => GameCopier.Copy(gameSource, gameDestination, Build("data/asset", "copied asset")));
        });

        test("Linux refuses a link in a sandbox sub-folder above a file SandboxStats reads", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux sandbox links)"); return; }
            var sandbox = Directory.CreateDirectory(Path.Combine(root, "sandbox-stats-link", "sandbox")).FullName;
            var outside = Directory.CreateDirectory(Path.Combine(root, "sandbox-stats-link", "outside")).FullName;
            File.WriteAllText(Path.Combine(outside, "planted.hd2rpreset.json"), """{"format":"hd2r-editor-preset"}""");
            Directory.CreateSymbolicLink(Path.Combine(sandbox, "presets"), outside);
            reject(() => SandboxStats.Apply(sandbox, Path.Combine(root, "sandbox-stats-link", "mods"), EntityProfile.Current));
        });

        test("Windows still refuses a link in any parent above the protected folder", () =>
        {
            if (!OperatingSystem.IsWindows()) { Console.WriteLine("  (skipped: Windows link rule)"); return; }
            var sandbox = Path.Combine(root, "windows-link-rule");
            var real = Directory.CreateDirectory(Path.Combine(sandbox, "real")).FullName;
            var linked = Path.Combine(sandbox, "linked");
            Directory.CreateSymbolicLink(linked, real);
            reject(() => LocalInstall.NoLinks(Path.Combine(linked, "HellForge"), Path.Combine(linked, "HellForge")));
            var plain = Directory.CreateDirectory(Path.Combine(real, "plain")).FullName;
            check(LocalInstall.NoLinks(plain, plain) == plain, "A plain Windows path was refused.");
        });
    }

    static (string Sandbox, string LinkedHome, string RealHome) Home(string root, string name)
    {
        var sandbox = Path.Combine(root, name);
        var realHome = Directory.CreateDirectory(Path.Combine(sandbox, "var", "home")).FullName;
        var linkedHome = Path.Combine(sandbox, "home");
        Directory.CreateSymbolicLink(linkedHome, realHome);
        return (sandbox, linkedHome, realHome);
    }

    static SupportedBuild Build(string relativePath, string text)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        var sha1 = Convert.ToHexString(SHA1.HashData(bytes));
        return new(1, LocalInstall.AppId, "12345", "", "", [new SupportedDepot(1, "1")],
            [new SupportedFile(relativePath, bytes.Length, sha1, 1)]);
    }
}
