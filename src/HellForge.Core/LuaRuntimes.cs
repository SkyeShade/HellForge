using System.Text.Json;

namespace HellForge.Core;

// The Lua runtimes Lua mods need, which players install themselves (HellForge bundles neither): Bingus Shared Loader runs
// every Lua mod, HD2Runtime the mods made for it (docs/lua-bridge.md). An enabled Lua mod without them does nothing, so
// the Mods page says so. HD2Runtime runs on HellForge only through HellForge's Lua bridge: one from before the bridge
// writes the game itself, over HellForge's own changes, and is reported too.
public static class LuaRuntimes
{
    public const string HD2RuntimeResource = "mods/skyeshade/hd2runtime";
    // The bridge an HD2Runtime build is made for ("hellforge": {"bridge": n} in its hd2runtime.json; hellforge_lua.h's
    // HF_LUA_BRIDGE_VERSION): one older than this HellForge's is refused by the engine's exports it binds.
    public const int Bridge = 3;

    // HD2R Editor (SkyeShade/HD2Runtime-Editor), HD2Runtime's own in-game editor, is a Lua mod made for HD2Runtime. It is not
    // supported on HellForge: it binds F8 like HellForge's editor, builds HD2Runtime's whole catalogue while the game loads,
    // and replays its saved session over HellForge's live edits. HellForge keeps it switched off (ModCatalog.LoadOne) and
    // says so; HD2Runtime and the other mods made for it still run. It is known by its hd2runtime.json resource or its guid.
    public const string EditorResource = "mods/skyeshade/hd2runtime_editor";
    public const string EditorGuid = "e4b62702-7e11-557e-8413-2b0facb4c1ff";
    public const string EditorUnsupported = "HD2R Editor is not supported on HellForge, so HellForge keeps it switched off. " +
        "Use HellForge's own editor instead: press F8 in game (the HellForge editor mod must be on). HD2Runtime and the Lua mods made for it still run.";

    public static bool IsEditor(string folder)
    {
        static string? Text(JsonElement root, string name) =>
            root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
        static bool Guid(string? text) => System.Guid.TryParse(text, out var g) && g == System.Guid.Parse(EditorGuid);
        try
        {
            var spec = Path.Combine(folder, "hd2runtime.json");
            if (File.Exists(spec))
            {
                using var document = JsonDocument.Parse(File.ReadAllText(spec));
                if (Text(document.RootElement, "resource") == EditorResource || Guid(Text(document.RootElement, "guid"))) return true;
            }
            var manifest = Path.Combine(folder, CosmeticPacks.ManifestName);
            if (File.Exists(manifest))
            {
                using var document = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
                if (Guid(Text(document.RootElement, "Guid"))) return true;
            }
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException) { }
        return false;
    }

    // A Lua mod's identity: Bingus Shared Loader (its BingusSharedLoader-manifest.json), HD2Runtime itself, or a mod made
    // for HD2Runtime (hd2runtime.json); none for any other.
    public sealed record Identity(bool Bingus, bool Runtime, bool NeedsRuntime, int Bridge);

    public static Identity Identify(string folder)
    {
        var bingus = File.Exists(Path.Combine(folder, "BingusSharedLoader-manifest.json"));
        var spec = Path.Combine(folder, "hd2runtime.json");
        if (!File.Exists(spec)) return new Identity(bingus, false, false, 0);
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(spec));
            var root = document.RootElement;
            var runtime = root.TryGetProperty("resource", out var resource) && resource.ValueKind == JsonValueKind.String &&
                resource.GetString() == HD2RuntimeResource;
            var needs = root.TryGetProperty("requires", out var requires) && requires.ValueKind == JsonValueKind.Object &&
                requires.TryGetProperty("hd2runtime", out _);
            var bridge = root.TryGetProperty("hellforge", out var hellforge) && hellforge.ValueKind == JsonValueKind.Object &&
                hellforge.TryGetProperty("bridge", out var b) && b.TryGetInt32(out var n) ? n : 0;
            return new Identity(bingus, runtime && !needs, needs, bridge);
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException)
        {
            return new Identity(bingus, false, false, 0);
        }
    }

    // What the enabled mods lack: Bingus Shared Loader for their Lua, HD2Runtime for its mods, HellForge's bridge in it.
    public static List<string> Problems(IEnumerable<ModManifest> enabled)
    {
        var mods = enabled.Select(m => (Mod: m, Is: Identify(m.Folder))).ToList();
        var problems = new List<string>();
        var bingus = mods.Any(m => m.Is.Bingus);
        var runtime = mods.FirstOrDefault(m => m.Is.Runtime);
        var lua = mods.Where(m => !m.Is.Bingus && (m.Mod.Lua || m.Is.Runtime || m.Is.NeedsRuntime)).Select(m => m.Mod.Name).ToList();
        if (!bingus && lua.Count > 0)
            problems.Add($"{Names(lua)} {(lua.Count == 1 ? "needs" : "need")} Bingus Shared Loader, a Lua mod you add yourself (Nexus Mods); without it their Lua does not run.");
        var forRuntime = mods.Where(m => m.Is.NeedsRuntime).Select(m => m.Mod.Name).ToList();
        if (runtime.Mod == null && forRuntime.Count > 0)
            problems.Add($"{Names(forRuntime)} {(forRuntime.Count == 1 ? "needs" : "need")} HD2Runtime, a Lua mod you add yourself.");
        if (runtime.Mod != null && runtime.Is.Bridge < Bridge)
            problems.Add($"{runtime.Mod.Name}: this HD2Runtime {(runtime.Is.Bridge == 0 ? "has no HellForge bridge" : $"was made for HellForge's bridge {runtime.Is.Bridge}, this HellForge has {Bridge}")}; " +
                "it would change the game itself, over HellForge's changes. Use an HD2Runtime made for this HellForge.");
        return problems;
    }

    static string Names(List<string> names) => names.Count <= 3 ? string.Join(", ", names) : $"{string.Join(", ", names.Take(3))} and {names.Count - 3} more";
}
