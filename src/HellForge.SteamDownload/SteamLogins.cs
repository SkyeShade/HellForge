using System.Text.Json;
using System.Text.Json.Serialization;

namespace HellForge.SteamDownload;

// Saved Steam sign-ins for the asset kit's fetches (docs/modding/assets.md "Signing in to Steam"): after one QR sign-in
// approved in the Steam app, the refresh token Steam returns is kept, so the next fetch signs in without the phone.
//   Own       <ApplicationData>/HellForge/steam-login.json ({accountName, refreshToken}; on Linux readable by
//             the user only, its folder too), written atomically, renewed when Steam offers a new token, deleted when
//             Steam no longer accepts it or the player signs out.
//   ReadOnly  other tools' saved sign-ins tried after it, never written or deleted: the asset migrator's
//             (<ApplicationData>/hellforge-migrator/steam-login.json, the same two fields in PascalCase; names are read
//             in any case).
// The token signs in to the player's whole Steam account; it never leaves this file, a log or a message (SecretScrubber).
// Setup's download keeps nothing (SignIn.cs).
public sealed class SteamLogins(string own, IReadOnlyList<string> readOnly)
{
    public sealed record Login(string AccountName, string RefreshToken, string From, bool Own);

    public string OwnPath { get; } = own;

    // HellForge's store, then the migrator's.
    public static SteamLogins Default { get; } = new(
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "HellForge", "steam-login.json"),
        [Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "hellforge-migrator", "steam-login.json")]);

    // Each saved sign-in to try, HellForge's first; one that cannot be read is left out.
    public IReadOnlyList<Login> Read()
    {
        var logins = new List<Login>();
        if (Load(OwnPath) is { } mine) logins.Add(new(mine.Account, mine.Token, "HellForge's saved sign-in", true));
        foreach (var path in readOnly)
            if (Load(path) is { } other && logins.All(l => l.RefreshToken != other.Token))
                logins.Add(new(other.Account, other.Token, "the asset migrator's saved sign-in", false));
        return logins;
    }

    public bool HasOwn => Load(OwnPath) != null;

    static (string Account, string Token)? Load(string path)
    {
        try
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 64 * 1024) return null;
            var stored = JsonSerializer.Deserialize(File.ReadAllText(path), SteamLoginJson.Default.StoredSteamLogin);
            return stored is { AccountName: { Length: > 0 } account, RefreshToken: { Length: > 0 } token } ? (account, token) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // Keeps a sign-in in HellForge's store, replacing what is there: the file is written beside it with the user's
    // permissions only, then moved over it.
    public void Save(string accountName, string refreshToken)
    {
        var folder = Path.GetDirectoryName(OwnPath)!;
        if (OperatingSystem.IsWindows()) Directory.CreateDirectory(folder);
        else Directory.CreateDirectory(folder, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var temporary = OwnPath + $".{Guid.NewGuid():N}.tmp";
        var options = new FileStreamOptions { Mode = FileMode.CreateNew, Access = FileAccess.Write };
        if (!OperatingSystem.IsWindows()) options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        try
        {
            using (var stream = new FileStream(temporary, options))
                JsonSerializer.Serialize(stream, new StoredSteamLogin(accountName, refreshToken), SteamLoginJson.Default.StoredSteamLogin);
            File.Move(temporary, OwnPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    // Signs out: HellForge's saved sign-in is deleted (true when there was one). Other tools' are theirs to remove.
    public bool Forget()
    {
        if (!File.Exists(OwnPath)) return false;
        File.Delete(OwnPath);
        return true;
    }

    // Steam no longer accepts this sign-in: HellForge's own is deleted (unless a newer one replaced it meanwhile).
    public void Rejected(Login login)
    {
        if (login.Own && Load(OwnPath) is { } current && current.Token == login.RefreshToken) Forget();
    }
}

// The file's form (the asset migrator's too).
sealed record StoredSteamLogin(
    [property: JsonPropertyName("accountName")] string? AccountName,
    [property: JsonPropertyName("refreshToken")] string? RefreshToken);

[JsonSerializable(typeof(StoredSteamLogin))]
[JsonSourceGenerationOptions(PropertyNameCaseInsensitive = true)]
sealed partial class SteamLoginJson : JsonSerializerContext;
