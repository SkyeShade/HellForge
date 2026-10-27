using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// HellForge's verified list: code and raw mods whose exact content a HellForge maintainer reviewed, which mod sync
// downloads from any host without asking. A version of a mod is listed by its content hash (ModSets.ContentHash;
// HellForge.Cli mod-hash prints it, docs/modding/tools.md "The verified list"), so any other bytes under the same id and
// version are not verified.
//   {"format":1,"serial":<n>,"mods":[{"id":"...","name":"...","version":"...","hash":"<64 hex>","reviewed":"<date>, <by whom>"}]}
// Two copies: the one embedded in Core (data/verified-mods.json, a release's), and the one published at Url with its
// signature at Url + ".sig", which the session runner fetches before each session (RefreshAsync) into the installation
// (SavedPath), so a mod is added or taken off without a release. The published list is the trust decision itself (its mods
// run from any host unasked), so it is taken only with a valid signature by the maintainers' key (PublicKey: ECDSA P-256
// over the file's exact bytes, SHA-256, IEEE P1363, base64), whose private half never goes to the server; every process
// checks it again when it reads the saved copy (UseInstallation). Serial only goes up: a list is used only when its serial
// is above the embedded list's, and a fetched one replaces the saved one only when its serial is not below it, so an old
// signed list cannot be served again to bring back a mod that was taken off (an installation that never saw a newer one can
// still be given an older one above its embedded list: each release raises the embedded serial).
public static class VerifiedMods
{
    public sealed record Entry(string Id, string Name, string Version, string Hash, string Reviewed);
    public sealed record Listing(int Serial, IReadOnlyList<Entry> Mods);
    public enum Source { Published, Saved, Embedded }

    public const string Url = "https://files.hf-mod.com/verified-mods/verified-mods.json", SignatureSuffix = ".sig", SavedName = "verified-mods.json";
    // The maintainers' signing key: SubjectPublicKeyInfo, base64 (HellForge.Cli verified-mods keygen prints it). Empty: no
    // published list is taken, only the embedded one.
    public const string PublicKey = "";
    public const int MaxBytes = 1 << 20;

    static readonly Lazy<Listing> embedded = new(() =>
    {
        using var stream = typeof(VerifiedMods).Assembly.GetManifestResourceStream("HellForge.Core.verified-mods.json")
            ?? throw new InvalidDataException("The verified mod list is not embedded.");
        using var reader = new StreamReader(stream);
        return ParseListing(reader.ReadToEnd());
    });
    static Listing? saved;

    public static Listing Embedded => embedded.Value;
    // The list in use: the saved published one when its serial is above the embedded one's.
    public static Listing Current => Volatile.Read(ref saved) is { } listing && listing.Serial > Embedded.Serial ? listing : Embedded;
    public static IReadOnlyList<Entry> All => Current.Mods;

    public static bool Contains(string id, string hash) => All.Any(e => e.Id == id && e.Hash == hash);

    public static string SavedPath(InstallLayout install) => Path.Combine(install.Temp, SavedName);

    // Takes the installation's saved copy (RefreshAsync's) when its signature checks out; the embedded list otherwise.
    public static Source UseInstallation(InstallLayout install, string publicKey = PublicKey)
    {
        var listing = ReadSaved(SavedPath(install), publicKey);
        Volatile.Write(ref saved, listing);
        return listing != null && listing.Serial > Embedded.Serial ? Source.Saved : Source.Embedded;
    }

    // Fetches the published list and its signature (within timeout), checks them and saves them for this installation's
    // processes. When it cannot (offline, not signed by the key, older than the saved one), the saved copy, else the
    // embedded list; Problem says why.
    public static async Task<(Source Source, string? Problem)> RefreshAsync(HttpClient http, InstallLayout install, TimeSpan timeout, string url = Url,
        string publicKey = PublicKey, CancellationToken cancellation = default)
    {
        var path = SavedPath(install);
        var before = ReadSaved(path, publicKey);
        string? problem = "HellForge has no key to check a published list with";
        if (publicKey.Length > 0)
            try
            {
                using var limit = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                limit.CancelAfter(timeout);
                var list = await http.GetByteArrayAsync(url, limit.Token);
                var signature = await http.GetStringAsync(url + SignatureSuffix, limit.Token);
                var listing = Checked(list, signature, publicKey);
                if (before != null && listing.Serial < before.Serial)
                    throw new InvalidDataException($"the published list's serial {listing.Serial} is below the saved one's ({before.Serial})");
                Save(path, list, signature);
                Volatile.Write(ref saved, listing);
                return (listing.Serial > Embedded.Serial ? Source.Published : Source.Embedded, null);
            }
            catch (Exception error) when (error is HttpRequestException or InvalidDataException or JsonException or FormatException or CryptographicException or InvalidOperationException or
                                              IOException or UnauthorizedAccessException ||
                                          error is OperationCanceledException && !cancellation.IsCancellationRequested)
            {
                problem = error is OperationCanceledException ? "no answer within " + timeout.TotalSeconds + " s" : error.Message;
            }
        Volatile.Write(ref saved, before);
        return (before != null && before.Serial > Embedded.Serial ? Source.Saved : Source.Embedded, problem);
    }

    // The list in those bytes, when signature (base64) is the key's signature of them; InvalidDataException otherwise.
    public static Listing Checked(byte[] list, string signature, string publicKey)
    {
        if (list.Length > MaxBytes) throw new InvalidDataException($"the list is larger than {MaxBytes} bytes");
        byte[] signed;
        try { signed = Convert.FromBase64String(signature.Trim()); }
        catch (FormatException) { throw new InvalidDataException("the list's signature is not base64"); }
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(Convert.FromBase64String(publicKey), out _);
        if (!key.VerifyData(list, signed, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation))
            throw new InvalidDataException("the list is not signed by HellForge's key");
        var listing = ParseListing(Encoding.UTF8.GetString(list).TrimStart('﻿'));
        return listing.Serial >= 1 ? listing : throw new InvalidDataException("a published list needs a serial of 1 or more");
    }

    // The saved copy: {"format":1,"list":"<the published file, base64>","signature":"<its signature>"}, written whole.
    static void Save(string path, byte[] list, string signature)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var text = new JsonObject { ["format"] = 1, ["list"] = Convert.ToBase64String(list), ["signature"] = signature.Trim() }.ToJsonString();
        File.WriteAllText(path + ".partial", text);
        File.Move(path + ".partial", path, overwrite: true);
    }

    static Listing? ReadSaved(string path, string publicKey)
    {
        if (publicKey.Length == 0) return null;
        try
        {
            if (!File.Exists(path) || new FileInfo(path).Length > MaxBytes * 2) return null;
            var root = JsonNode.Parse(File.ReadAllText(path)) as JsonObject;
            if ((int?)root?["format"] != 1 || (string?)root["list"] is not { } list || (string?)root["signature"] is not { } signature) return null;
            return Checked(Convert.FromBase64String(list), signature, publicKey);
        }
        catch (Exception error) when (error is InvalidDataException or JsonException or FormatException or CryptographicException or InvalidOperationException or
                                          IOException or UnauthorizedAccessException) { return null; }
    }

    // Maintainers' side (HellForge.Cli verified-mods): a new key pair (the private key as PKCS#8 PEM, the public key as
    // PublicKey wants it), and a list's signature with the private key.
    public static (string PrivateKeyPem, string PublicKey) NewKey()
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        return (key.ExportPkcs8PrivateKeyPem(), Convert.ToBase64String(key.ExportSubjectPublicKeyInfo()));
    }

    public static string Sign(byte[] list, string privateKeyPem)
    {
        using var key = ECDsa.Create();
        key.ImportFromPem(privateKeyPem);
        if (key.KeySize != 256) throw new InvalidDataException("The signing key is not a P-256 key.");
        return Convert.ToBase64String(key.SignData(list, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
    }

    // The public key (PublicKey's form) of a private key.
    public static string PublicKeyOf(string privateKeyPem)
    {
        using var key = ECDsa.Create();
        key.ImportFromPem(privateKeyPem);
        return Convert.ToBase64String(key.ExportSubjectPublicKeyInfo());
    }

    public static IReadOnlyList<Entry> Parse(string json) => ParseListing(json).Mods;

    // serial: 0 when the list has none (an embedded list from before it).
    public static Listing ParseListing(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The verified mod list is not an object.");
        if ((int?)root["format"] != 1 || root["mods"] is not JsonArray mods) throw new InvalidDataException("The verified mod list has no format 1 or no mods.");
        var serial = root["serial"] is null ? 0 : root["serial"] is JsonValue value && value.TryGetValue<int>(out var number) && number >= 0 ? number
            : throw new InvalidDataException("The verified mod list's serial is not a whole number.");
        var entries = new List<Entry>();
        foreach (var node in mods)
        {
            var entry = new Entry((string?)node?["id"] ?? "", (string?)node?["name"] ?? "", (string?)node?["version"] ?? "", (string?)node?["hash"] ?? "",
                (string?)node?["reviewed"] ?? "");
            if (!ModCatalog.IsModId(entry.Id) || !ModSets.Hex(entry.Hash, 64) || entry.Version.Length == 0 || entry.Reviewed.Length == 0)
                throw new InvalidDataException($"A verified mod entry is malformed: {entry.Id}.");
            entries.Add(entry);
        }
        return new Listing(serial, entries);
    }
}
