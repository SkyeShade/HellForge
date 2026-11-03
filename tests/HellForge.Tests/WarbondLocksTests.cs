using System.Text.Json;
using HellForge.Core;
using HellForge.Core.GameData;

// The locked warbond art (WarbondLocks): the mapping against the progression data, the look drawn on the art, and its
// patch built from a synthetic game data folder and staged after the mods'.
static class WarbondLocksTests
{
    const string Archive = "4b7bd0cbf5b59fcf";

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Every warbond with locked art is a premium warbond of the progression data, its textures named once", () =>
        {
            using var data = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(Repository(), "data", "progression-data.json")));
            var details = data.RootElement.GetProperty("seasonPassDetails");
            var mapping = WarbondLocks.Mapping();
            var premium = details.EnumerateObject().Count(d => d.Name != PremiumContent.FreeWarbond.ToString(System.Globalization.CultureInfo.InvariantCulture));
            check(mapping.Count == premium, $"The mapping has {mapping.Count} warbonds, the progression data {premium} premium ones.");
            foreach (var art in mapping)
                check(art.Id != PremiumContent.FreeWarbond && details.TryGetProperty(art.Id.ToString(System.Globalization.CultureInfo.InvariantCulture), out _) &&
                    art.Textures.Length >= 1 && art.Name.Length > 0, $"{art.Name} ({art.Id}) is not a premium warbond of the progression data.");
            var textures = mapping.SelectMany(a => a.Textures).ToList();
            check(textures.Distinct().Count() == textures.Count, "A texture is named twice.");
        });

        test("Locked art is dimmed and grey, with a padlock in the middle and lines from the corners that stop short of it", () =>
        {
            const int w = 2048, h = 1024;
            var rgba = new byte[w * h * 4];
            for (var i = 0; i < rgba.Length; i += 4) (rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]) = (200, 40, 40, 255);
            WarbondLocks.Draw(rgba, w, h);
            (int R, int G, int B) At(int x, int y) => (rgba[4 * (y * w + x)], rgba[4 * (y * w + x) + 1], rgba[4 * (y * w + x) + 2]);
            bool Dark((int R, int G, int B) p) => p.R < 90 && p.R - p.G < 50;
            bool Light((int R, int G, int B) p) => p.R > 180 && p.G > 180 && p.B > 180;
            check(Dark(At(5, h / 2)) && Dark(At(w / 2, 40)), "The art was not dimmed and greyed.");
            check(Light(At(w / 2 - 70, h / 2 + 90)), "No padlock body.");
            check(Light(At(w / 2, h / 2 - 117)) && Light(At(w / 2 - 62, h / 2 - 60)), "No shackle.");
            check(Dark(At(w / 2, h / 2 + 28)), "No keyhole.");
            check(Dark(At(w / 2, h / 2 - 80)), "The shackle is filled in.");
            // On the diagonal from the top-left corner: drawn far out, clear close to the padlock.
            var (dx, dy) = (w / 2f, h / 2f);
            var length = MathF.Sqrt(dx * dx + dy * dy);
            (int, int) Along(float from) => ((int)(w / 2f - dx * from / length), (int)(h / 2f - dy * from / length));
            var (fx, fy) = Along(600);
            check(At(fx, fy).R > 120, "No line from the corner.");
            var (cx, cy) = Along(185);
            check(Dark(At(cx, cy)), "The line runs into the padlock.");
        });

        test("Locked cards are built into a patch of their archive; other textures and missing cards are left alone", () =>
        {
            var folder = Path.Combine(root, "warbond-locks");
            var gameData = Path.Combine(folder, "game", "data");
            const int w = 64, h = 32;
            var card = new ResourceParts(IconImage.TextureMain(w, h, 1), Enumerable.Repeat((byte)0x55, w * h / 2).ToArray(), []);
            var mipped = new ResourceParts(IconImage.TextureMain(w, h, 2), new byte[w * h / 2 + w * h / 8], []);
            var archive = ResourceArchive.Write(new Dictionary<ResourceKey, ResourceParts>
            {
                [new(ResourceTypes.Texture, 0xC0DE)] = card,
                [new(ResourceTypes.Texture, 0xBEEF)] = mipped,
            });
            DataBuilderTests.WriteBundles(gameData, new Dictionary<string, byte[]> { [Archive] = archive.Archive, [Archive + ".gpu_resources"] = archive.Gpu });
            var output = Path.Combine(folder, "out");
            using (var game = new GameDataReader(gameData))
            {
                var files = WarbondLocks.Build(game, [0xC0DE, 0xBEEF, 0xF00D], output);
                check(files.SequenceEqual([Archive + ".patch_0", Archive + ".patch_0.gpu_resources"]), "Patch files: " + string.Join(", ", files));
            }
            var patch = ResourceArchive.Read(File.ReadAllBytes(Path.Combine(output, Archive + ".patch_0")),
                File.ReadAllBytes(Path.Combine(output, Archive + ".patch_0.gpu_resources")), []);
            check(patch.Count == 1 && patch.TryGetValue(new(ResourceTypes.Texture, 0xC0DE), out var locked) && locked.Main.SequenceEqual(card.Main) &&
                locked.Gpu.Length == card.Gpu.Length && !locked.Gpu.SequenceEqual(card.Gpu), "The card was not locked in place, or another texture was.");

            // HellForge's own patches go after every mod's.
            var install = new InstallLayout(Path.Combine(folder, "install"));
            install.Create("12345");
            var data = Path.Combine(install.Game, "data");
            Directory.CreateDirectory(data);
            File.WriteAllText(Path.Combine(data, Archive + ".patch_0"), "installed by hand");
            var staged = ModStaging.Stage(install, [], [output]);
            check(staged.SequenceEqual([Archive + ".patch_1", Archive + ".patch_1.gpu_resources"]), "Staged: " + string.Join(", ", staged));
            ModStaging.Unstage(install);
        });
    }

    static string Repository()
    {
        var folder = AppContext.BaseDirectory;
        while (!File.Exists(Path.Combine(folder, "HellForge.slnx"))) folder = Path.GetDirectoryName(folder.TrimEnd(Path.DirectorySeparatorChar))!;
        return folder;
    }
}
