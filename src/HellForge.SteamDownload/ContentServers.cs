using System.Net;
using HellForge.Core;
using SteamKit2;
using SteamKit2.CDN;
using DepotChunk = HellForge.Core.DepotChunk;

namespace HellForge.SteamDownload;

// Steam's content servers for this session, best first (Steam's own load figure), and the requests made on them: a
// server that fails is moved to the back and the request tries the next, up to five times. A server that refuses a depot
// without a CDN auth token is asked again with one from Steam (kept per depot and host).
sealed class ContentServers(SteamContent content)
{
    readonly SemaphoreSlim loading = new(1, 1);
    readonly Dictionary<(uint Depot, string Host), string> tokens = [];
    List<Server>? list;

    public async Task<T> Try<T>(uint app, uint depot, uint cellId, Func<Server, string?, CancellationToken, Task<T>> request, CancellationToken cancellation)
    {
        for (var attempt = 1; ; attempt++)
        {
            var server = await Next(app, cellId, cancellation);
            try { return await request(server, TokenFor(depot, server), cancellation); }
            catch (SteamKitWebRequestException error) when (error.StatusCode is HttpStatusCode.Forbidden or HttpStatusCode.Unauthorized && attempt < 5)
            {
                // the same server again, with the token
                await Token(app, depot, server);
            }
            catch (Exception error) when (Transient(error, cancellation) && attempt < 5)
            {
                Demote(server);
                await Task.Delay(TimeSpan.FromMilliseconds(250 * attempt), cancellation);
            }
        }
    }

    static bool Transient(Exception error, CancellationToken cancellation) =>
        error is HttpRequestException or IOException or TimeoutException or InvalidDataException ||
        error is OperationCanceledException && !cancellation.IsCancellationRequested;

    async Task<Server> Next(uint app, uint cellId, CancellationToken cancellation)
    {
        await loading.WaitAsync(cancellation);
        try
        {
            if (list is not { Count: > 0 })
            {
                var all = await content.GetServersForSteamPipe(cellId, 20).WaitAsync(TimeSpan.FromSeconds(30), cancellation);
                // Steam's own caches and CDNs first; a proxy only rewrites requests for another server, so it is left out
                list = [.. all.Where(s => !s.UseAsProxy && !s.SteamChinaOnly && (s.AllowedAppIds is not { Length: > 0 } || s.AllowedAppIds.Contains(app)))
                    .OrderBy(s => s.Type is "SteamCache" or "CDN" ? 0 : 1).ThenBy(s => s.WeightedLoad)];
                if (list.Count == 0) throw new IOException("Steam named no content server to download from.");
            }
            lock (list) return list[0];
        }
        finally { loading.Release(); }
    }

    void Demote(Server server)
    {
        if (list == null) return;
        lock (list)
            if (list.Remove(server)) list.Add(server);
    }

    async Task Token(uint app, uint depot, Server server)
    {
        var token = await content.GetCDNAuthToken(app, depot, server.Host!).WaitAsync(TimeSpan.FromSeconds(30));
        if (token.Result == EResult.OK && token.Token is { Length: > 0 }) lock (tokens) tokens[(depot, server.Host!)] = token.Token;
    }

    string? TokenFor(uint depot, Server server)
    {
        lock (tokens) return tokens.GetValueOrDefault((depot, server.Host ?? ""));
    }
}

// Steam's manifest as SteamKit2 reads it, in HellForge.Core's form (the same one Steam's cached manifests are read into).
static class ManifestFormat
{
    public static Core.DepotManifest From(SteamKit2.DepotManifest manifest) => new(manifest.DepotID, manifest.ManifestGID, manifest.FilenamesEncrypted,
        [.. (manifest.Files ?? []).Select(file => new DepotFile((file.FileName ?? "").Replace('\\', '/'), file.TotalSize, (uint)file.Flags,
            Convert.ToHexString(file.FileHash ?? []), [.. file.Chunks.Select(c => new DepotChunk(Convert.ToHexString(c.ChunkID ?? []), c.Checksum, c.Offset,
                c.UncompressedLength, c.CompressedLength))]))]);

    public static SteamKit2.DepotManifest.ChunkData ToSteam(DepotChunk chunk) =>
        new(Convert.FromHexString(chunk.Sha1), chunk.Crc, chunk.Offset, chunk.CompressedSize, chunk.OriginalSize);
}
