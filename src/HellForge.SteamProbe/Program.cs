using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using HellForge.Core;

if (!OperatingSystem.IsWindows() || args.Length != 3)
{
    Console.Error.WriteLine("Use HellForge.SteamProbe <verified steam_api64.dll> <expected SHA256> <report.json>.");
    return 1;
}
var libraryPath = Path.GetFullPath(args[0]);
var reportPath = Path.GetFullPath(args[2]);
try
{
    using var lockedLibrary = new FileStream(libraryPath, FileMode.Open, FileAccess.Read, FileShare.Read);
    if (!Convert.ToHexString(SHA256.HashData(lockedLibrary)).Equals(args[1], StringComparison.OrdinalIgnoreCase))
        throw new IOException("Steam API image changed after verification.");
    var library = NativeLibrary.Load(libraryPath);
    var initialized = false;
    try
    {
        T Export<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));
        // Never call RestartAppIfNecessary, ticket creation, stats, cloud, or launch APIs.
        initialized = Export<NoArgBool>("SteamAPI_Init")();
        if (!initialized) throw new IOException("SteamAPI_Init failed; ownership remains unverified.");
        var apps = Export<Interface>("SteamAPI_SteamApps_v008")();
        var user = Export<Interface>("SteamAPI_SteamUser_v021")();
        var utils = Export<Interface>("SteamAPI_SteamUtils_v010")();
        if (apps == 0 || user == 0 || utils == 0) throw new IOException("Required Steam interfaces unavailable.");
        var appId = Export<UInt32Call>("SteamAPI_ISteamUtils_GetAppID")(utils);
        var loggedOn = Export<BoolCall>("SteamAPI_ISteamUser_BLoggedOn")(user);
        var subscribed = Export<BoolCall>("SteamAPI_ISteamApps_BIsSubscribed")(apps);
        var shared = Export<BoolCall>("SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing")(apps);
        var freeWeekend = Export<BoolCall>("SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend")(apps);
        var cafe = Export<BoolCall>("SteamAPI_ISteamApps_BIsCybercafe")(apps);
        var userId = Export<UInt64Call>("SteamAPI_ISteamUser_GetSteamID")(user);
        var ownerId = Export<UInt64Call>("SteamAPI_ISteamApps_GetAppOwner")(apps);
        var sameOwner = userId != 0 && ownerId == userId;
        var confirmed = SteamOwnershipPolicy.ConfirmsCurrentClientOwner(new(appId, loggedOn, subscribed, sameOwner, shared, freeWeekend, cafe));
        File.WriteAllText(reportPath, JsonSerializer.Serialize(new {
            status = confirmed ? "local_steam_client_subscription_confirmed" : "ownership_unverified",
            checkedUtc = DateTime.UtcNow, appId, loggedOn, subscribed, sameOwner, familyShared = shared, freeWeekend, cafe,
            localClientOwnershipConfirmed = confirmed,
            accountHash = userId == 0 ? null : SteamOwnershipCheck.AccountHash(unchecked((uint)userId)), serverVerified = false, permanentLicenseProven = false,
            readyToLaunchGame = false, librarySha256 = args[1],
            scope = "Live client API subscription, and owner match or Steam Family Sharing. Not a signed portable receipt or publisher-side verification."
        }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine(confirmed ? "Local Steam client subscription and owner match confirmed." : "Ownership remains unverified.");
        return confirmed ? 0 : 2;
    }
    finally
    {
        if (initialized) Marshal.GetDelegateForFunctionPointer<Shutdown>(NativeLibrary.GetExport(library, "SteamAPI_Shutdown"))();
        NativeLibrary.Free(library);
    }
}
catch (Exception error)
{
    File.WriteAllText(reportPath, JsonSerializer.Serialize(new { status = "ownership_unverified", readyToLaunchGame = false, failure = error.Message }));
    Console.Error.WriteLine(error.Message);
    return 1;
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
[return: MarshalAs(UnmanagedType.I1)]
internal delegate bool NoArgBool();
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
[return: MarshalAs(UnmanagedType.I1)]
internal delegate bool BoolCall(nint self);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate nint Interface();
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate uint UInt32Call(nint self);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate ulong UInt64Call(nint self);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void Shutdown();
