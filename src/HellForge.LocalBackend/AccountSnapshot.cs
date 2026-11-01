using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.LocalBackend;

// A profile's account snapshot (HellForge.Core InventorySync) as the backend trusts it: only when its seal holds on this PC
// and names the Steam account signed in now (SnapshotSeal). Premium ownership and imports read nothing else.
public sealed class AccountSnapshot : IDisposable
{
    AccountSnapshot(JsonDocument inventory, JsonDocument? progression, SnapshotSeal.Info seal)
    {
        Inventory = inventory;
        Progression = progression;
        Seal = seal;
    }

    public JsonDocument Inventory { get; }
    public JsonDocument? Progression { get; }
    public SnapshotSeal.Info Seal { get; }

    // Null, with the reason in problem, unless the snapshot is complete, sealed here and for steamAccount.
    public static AccountSnapshot? Open(string snapshotDir, ulong? steamAccount, out string? problem)
    {
        problem = Refusal(snapshotDir, steamAccount, out var seal);
        if (problem != null) return null;
        JsonDocument? inventory = null;
        try
        {
            inventory = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(snapshotDir, InventorySync.InventoryFile)));
            if (!inventory.RootElement.TryGetProperty("items", out var items) || items.ValueKind != JsonValueKind.Array)
                throw new JsonException("no items list");
            var progressionPath = Path.Combine(snapshotDir, InventorySync.ProgressionFile);
            var progression = File.Exists(progressionPath) ? JsonDocument.Parse(File.ReadAllBytes(progressionPath)) : null;
            return new AccountSnapshot(inventory, progression, seal!);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            inventory?.Dispose();
            problem = "the snapshot cannot be read (" + error.Message + ")";
            return null;
        }
    }

    static string? Refusal(string snapshotDir, ulong? steamAccount, out SnapshotSeal.Info? seal)
    {
        seal = null;
        if (!InventorySync.HasSnapshot(snapshotDir)) return "there is no snapshot";
        seal = SnapshotSeal.Verify(snapshotDir, InventorySync.SealKeyFor(snapshotDir));
        if (seal == null) return "the snapshot changed since it was captured, or was captured on another PC";
        if (steamAccount == null) return "Steam is not signed in";
        return seal.SteamAccount == steamAccount ? null : "the snapshot was captured for another Steam account";
    }

    public IEnumerable<JsonObject> Items() =>
        Inventory.RootElement.GetProperty("items").EnumerateArray()
            .Where(item => item.ValueKind == JsonValueKind.Object)
            .Select(item => JsonNode.Parse(item.GetRawText())!.AsObject())
            .ToList();

    public void Dispose()
    {
        Inventory.Dispose();
        Progression?.Dispose();
    }
}
