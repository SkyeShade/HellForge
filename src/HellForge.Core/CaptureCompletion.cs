namespace HellForge.Core;

// Sync Inventory's capture is passive: it keeps the API's answers to the game's own reads after its login and never sends
// a request itself. The game reads the catalogues (GameCatalog) and the progression, then the inventory, then the
// warbond list (live run 2026-11-06). Kept: successful answers after this run's login, each once. Complete: the inventory
// with everything asked for, or after ResponseWait without what is still missing. Every member locks (concurrent
// connections). The capture compiles this file in, so it uses nothing else from HellForge.Core.
public sealed class CaptureCompletion(bool progressionWanted, bool catalogueWanted = false)
{
    public enum Kind { Other, Login, Progression, Inventory, Catalogue }

    public const string LoginPath = "/api/Account/Login", ProgressionPath = "/api/Progression", InventoryPath = "/api/Progression/inventory";

    // The catalogues the game reads after its login, and the file each is kept as (the request's own last segment).
    public static readonly IReadOnlyList<(string Path, string File)> Catalogues =
    [
        ("/api/Progression/ItemPackages", "ItemPackages.json"),
        ("/api/Progression/ProgressionPackages", "ProgressionPackages.json"),
        ("/api/Progression/items", "items.json"),
        ("/api/Progression/levelspec", "levelspec.json"),
        ("/api/SeasonPass", "SeasonPass.json")
    ];

    public static readonly TimeSpan ResponseWait = TimeSpan.FromSeconds(15);

    readonly object gate = new();
    readonly HashSet<string> catalogues = new(StringComparer.Ordinal);
    bool loggedIn, progression, inventory, completed;
    DateTime inventoryUtc;

    // A request the capture cares about, by its method and its path without the query.
    public static Kind Classify(string method, string path)
    {
        if (method == "POST" && path.Equals(LoginPath, StringComparison.OrdinalIgnoreCase)) return Kind.Login;
        if (method != "GET") return Kind.Other;
        if (path.Equals(ProgressionPath, StringComparison.OrdinalIgnoreCase)) return Kind.Progression;
        if (path.Equals(InventoryPath, StringComparison.OrdinalIgnoreCase)) return Kind.Inventory;
        return CatalogueFile(path) != null ? Kind.Catalogue : Kind.Other;
    }

    // The file a catalogue read is kept as, or null when the path is not one of Catalogues.
    public static string? CatalogueFile(string path)
    {
        foreach (var (cataloguePath, file) in Catalogues)
            if (path.Equals(cataloguePath, StringComparison.OrdinalIgnoreCase)) return file;
        return null;
    }

    public bool LoggedIn { get { lock (gate) return loggedIn; } }
    public bool HasProgression { get { lock (gate) return progression; } }
    public bool HasInventory { get { lock (gate) return inventory; } }
    public bool HasCatalogues { get { lock (gate) return catalogues.Count == Catalogues.Count; } }
    public bool Completed { get { lock (gate) return completed; } }

    // What is still missing once the inventory is in, for the log; empty when nothing is.
    public IReadOnlyList<string> Missing()
    {
        lock (gate)
        {
            var missing = new List<string>();
            if (progressionWanted && !progression) missing.Add("progression");
            if (catalogueWanted)
                missing.AddRange(Catalogues.Where(entry => !catalogues.Contains(entry.File)).Select(entry => entry.File));
            return missing;
        }
    }

    // A successful login answer: the game's reads after it are this run's account.
    public void Login() { lock (gate) loggedIn = true; }

    // Why an answer of this kind is not kept, or null when it should be saved now. A catalogue's path names which one.
    public string? Refusal(Kind kind, bool success, string? path = null)
    {
        lock (gate)
        {
            if (kind is not (Kind.Progression or Kind.Inventory or Kind.Catalogue)) return "not a read the capture keeps";
            if (!success) return "the API refused it";
            if (completed) return "the capture is already complete";
            if (!loggedIn) return "it came before a login";
            return kind switch
            {
                Kind.Catalogue => CatalogueRefusal(path),
                Kind.Progression when !progressionWanted => "the progression was not asked for",
                Kind.Progression when progression => "it was already kept",
                Kind.Inventory when inventory => "it was already kept",
                _ => null
            };
        }
    }

    // Called under the lock.
    string? CatalogueRefusal(string? path)
    {
        if (!catalogueWanted) return "the catalogues were not asked for";
        if ((path != null ? CatalogueFile(path) : null) is not { } file) return "not a catalogue the capture keeps";
        return catalogues.Contains(file) ? "it was already kept" : null;
    }

    // The answer was written.
    public void Saved(Kind kind, DateTime nowUtc, string? path = null)
    {
        lock (gate)
        {
            if (kind == Kind.Progression) progression = true;
            else if (kind == Kind.Catalogue && path != null && CatalogueFile(path) is { } file) catalogues.Add(file);
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
            var waiting = progressionWanted && !progression || catalogueWanted && catalogues.Count < Catalogues.Count;
            if (waiting && nowUtc - inventoryUtc < ResponseWait) return false;
            completed = true;
            return true;
        }
    }
}
