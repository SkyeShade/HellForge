namespace HellForge.Core;

// Sync Inventory's capture is passive: it keeps the answers to the game's own requests and never asks the API anything
// itself. After its login the live game reads GET /api/Progression and then GET /api/Progression/inventory on its own,
// about 1.5 s later (both Linux syncs, 2026-10-29 and 2026-11-04: the same sizes the capture's own reads had got moments
// before). This class decides what of that traffic is kept and when the capture is complete:
// - only a successful answer, only after a successful login of this run, and each of the two once;
// - complete once the inventory is in and the progression too, or when the progression was not asked for, or when it
//   has still not come ProgressionWait after the inventory (the capture is then sealed without it, as before).
// The game reads the progression first, so the capture normally completes on the inventory. The proxy calls this from
// concurrent connections, so every member locks.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public sealed class CaptureCompletion(bool progressionWanted)
{
    public enum Kind { Other, Login, Progression, Inventory }

    public const string LoginPath = "/api/Account/Login", ProgressionPath = "/api/Progression", InventoryPath = "/api/Progression/inventory";

    public static readonly TimeSpan ProgressionWait = TimeSpan.FromSeconds(15);

    readonly object gate = new();
    bool loggedIn, progression, inventory, completed;
    DateTime inventoryUtc;

    // A request the capture cares about, by its method and its path without the query.
    public static Kind Classify(string method, string path)
    {
        if (method == "POST" && path.Equals(LoginPath, StringComparison.OrdinalIgnoreCase)) return Kind.Login;
        if (method != "GET") return Kind.Other;
        if (path.Equals(ProgressionPath, StringComparison.OrdinalIgnoreCase)) return Kind.Progression;
        if (path.Equals(InventoryPath, StringComparison.OrdinalIgnoreCase)) return Kind.Inventory;
        return Kind.Other;
    }

    public bool LoggedIn { get { lock (gate) return loggedIn; } }
    public bool HasProgression { get { lock (gate) return progression; } }
    public bool HasInventory { get { lock (gate) return inventory; } }
    public bool Completed { get { lock (gate) return completed; } }

    // A successful login answer: the game's reads after it are this run's account.
    public void Login() { lock (gate) loggedIn = true; }

    // Why an answer of this kind is not kept, or null when it should be saved now.
    public string? Refusal(Kind kind, bool success)
    {
        lock (gate)
        {
            if (kind is not (Kind.Progression or Kind.Inventory)) return "not a read the capture keeps";
            if (!success) return "the API refused it";
            if (completed) return "the capture is already complete";
            if (!loggedIn) return "it came before a login";
            if (kind == Kind.Progression && !progressionWanted) return "the progression was not asked for";
            if (kind == Kind.Progression ? progression : inventory) return "it was already kept";
            return null;
        }
    }

    // The answer was written.
    public void Saved(Kind kind, DateTime nowUtc)
    {
        lock (gate)
        {
            if (kind == Kind.Progression) progression = true;
            else if (kind == Kind.Inventory && !inventory)
            {
                inventory = true;
                inventoryUtc = nowUtc;
            }
        }
    }

    // True exactly once: when the capture is complete and should be sealed now.
    public bool TryComplete(DateTime nowUtc)
    {
        lock (gate)
        {
            if (completed || !inventory) return false;
            if (progressionWanted && !progression && nowUtc - inventoryUtc < ProgressionWait) return false;
            completed = true;
            return true;
        }
    }
}
