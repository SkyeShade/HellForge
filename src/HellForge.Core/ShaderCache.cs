using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The game's shader cache (userdata/shader_cache: its D3D12 pipeline library, about 110 MB). Without one the game builds
// it behind a black screen for minutes. The game writes it only when it closes by itself: a game that is killed keeps
// the cache it started with (sessions ended with STOP never updated it, 2026-10-26), so a session asks the game to close
// first (GameClose). A sandboxed session takes a copy of it and brings it back (SandboxFolders).
public static class ShaderCache
{
    public const string Folder = "shader_cache";

    public sealed record Facts(int Files, long Bytes, DateTime? NewestUtc)
    {
        public bool Empty => Files == 0;

        // For result.json, which Save logs to Desktop zips.
        public JsonObject ToJson() => new()
        {
            ["files"] = Files, ["bytes"] = Bytes, ["newestUtc"] = NewestUtc?.ToString("o")
        };
    }

    // The cache under a user data root: its regular files, never through a link.
    public static Facts Read(string userDataRoot)
    {
        var folder = new DirectoryInfo(Path.Combine(userDataRoot, Folder));
        int files = 0;
        long bytes = 0;
        DateTime? newest = null;
        if (!folder.Exists || folder.Attributes.HasFlag(FileAttributes.ReparsePoint)) return new(0, 0, null);
        foreach (var file in folder.EnumerateFiles("*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint }))
        {
            files++;
            bytes += file.Length;
            if (newest == null || file.LastWriteTimeUtc > newest) newest = file.LastWriteTimeUtc;
        }
        return new(files, bytes, newest);
    }

    // An installation without a shader cache starts from the normal game's (%APPDATA%/Arrowhead/Helldivers2/shader_cache,
    // GameSettings.VanillaUserData): the same game on the same PC, so most of it is what this game would build. The game
    // checks the cache itself (a pipeline library made by another driver or build is not used: its entries are built
    // again). The normal game's folder is only read; regular files only, bounded like a sandbox copy. Null when the
    // installation has a cache already or the normal game has none.
    public static SandboxFolders.Copied? Seed(string userDataRoot, string? vanillaUserData)
    {
        if (!Read(userDataRoot).Empty || vanillaUserData == null || Read(vanillaUserData).Empty) return null;
        return SandboxFolders.Copy(vanillaUserData, userDataRoot, [Folder]);
    }

    // Whether a running session's game is most likely building its shader cache: it started without one (result.json
    // shaderCache.atStart) and has not asked the backend for anything yet (firstBackendRequestSeconds), which it does as
    // soon as it is past its black screen.
    public static bool Building(JsonElement state) =>
        state.ValueKind == JsonValueKind.Object &&
        state.TryGetProperty("shaderCache", out var cache) && cache.ValueKind == JsonValueKind.Object &&
        cache.TryGetProperty("atStart", out var start) && start.ValueKind == JsonValueKind.Object &&
        start.TryGetProperty("files", out var files) && files.ValueKind == JsonValueKind.Number && files.GetInt32() == 0 &&
        !state.TryGetProperty("firstBackendRequestSeconds", out _);
}
