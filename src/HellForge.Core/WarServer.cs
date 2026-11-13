using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace HellForge.Core;

// The HellForge war server a session follows (backend --war hellforge, docs/hellforge-war.md): a service of its own,
// with its endpoints at its root (/v1/snapshot, /v1/mission/...). Which one: HELLFORGE_WAR_SERVER, else the launcher's
// setting (launcher-settings.json "warServer"), else Default. An address is https, or http to this PC only (a server
// being tried locally), with no query, and is kept without its trailing slashes, so one server has one spelling.
public static class WarServer
{
    // PLACEHOLDER until the official HellForge war server has its address: a war server run on this PC (its local default).
    public const string Default = "http://127.0.0.1:5093", EnvironmentVariable = "HELLFORGE_WAR_SERVER";

    // The address as kept, or null when it is not one a session may use.
    public static string? Normalize(string? address)
    {
        if (string.IsNullOrWhiteSpace(address) || !Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri)) return null;
        if (uri.Query.Length > 0 || uri.Fragment.Length > 0 || uri.UserInfo.Length > 0) return null;
        var local = uri.IsLoopback || uri.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase);
        if (uri.Scheme != Uri.UriSchemeHttps && !(uri.Scheme == Uri.UriSchemeHttp && local)) return null;
        return uri.GetLeftPart(UriPartial.Path).TrimEnd('/');
    }

    // Each server's war is cached apart (hellforge-war/<host>-<hash>.json in the host's folder), so switching servers
    // never serves another server's war.
    public static string CachePath(string hostRoot, string server)
    {
        var uri = new Uri(server);
        var hash = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(server)))[..12];
        var host = new string([.. uri.Host.Select(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '.' ? c : '_')]);
        return Path.Combine(hostRoot, "hellforge-war", string.Create(CultureInfo.InvariantCulture, $"{host}-{hash}.json"));
    }
}
