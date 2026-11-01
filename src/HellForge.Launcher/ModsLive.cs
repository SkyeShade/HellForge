using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Launcher;

// The Mods page while a session runs (docs/mods.md "Changing mods while you play"): a switch, or a mod's files changing,
// goes to the running game at once when the game can take it (stats mods for now). The session's backend plans and makes
// the change (POST /hellforge/live-mods, HellForge-Engine LiveMods); anything else is saved for the next session, as
// before. The order, packs' options and deleting downloaded mods stay locked while a session runs.
public partial class MainWindow
{
    static readonly HttpClient LiveHttp = new(new HttpClientHandler { UseProxy = false }) { Timeout = TimeSpan.FromSeconds(60) };
    string? liveBackend;   // the running session's backend (result.json "backendOrigin"), once it is ready
    bool liveApplying, liveAgain;
    string liveStatus = "";  // the last change's outcome, under the page's summary

    // Called with each result.json while the session runs, and with null once it has ended.
    void SetLiveBackend(string? origin)
    {
        var known = origin is { Length: > 0 } && Uri.TryCreate(origin, UriKind.Absolute, out var uri) && uri.IsLoopback ? origin : null;
        if (known == liveBackend) return;
        liveBackend = known;
        if (known == null) liveStatus = "";
        SetModsLocked(modsLocked);
    }

    // Whether a switch can change while the session runs.
    bool LiveSwitches => modsLocked && liveBackend != null;

    // Sends the Mods page's mods to the running game. One change at a time; a change asked for meanwhile goes after it.
    async void ApplyLive()
    {
        if (!runner.IsRunning || liveBackend == null) return;
        if (liveApplying) { liveAgain = true; return; }
        liveApplying = true;
        try
        {
            do
            {
                liveAgain = false;
                using var reply = await LiveHttp.PostAsync(liveBackend + "/hellforge/live-mods", new StringContent("{}", Encoding.UTF8, "application/json"));
                var answer = JsonNode.Parse(await reply.Content.ReadAsStringAsync()) as JsonObject;
                if (!reply.IsSuccessStatusCode || answer == null)
                {
                    liveStatus = "The running game cannot take mod changes; they apply to the next session.";
                    continue;
                }
                ShowLiveResult(answer);
            } while (liveAgain && runner.IsRunning);
        }
        catch (Exception error) when (error is HttpRequestException or TaskCanceledException or JsonException)
        {
            liveStatus = "Could not reach the session to change its mods (" + error.Message + "); the changes apply to the next session.";
            Append(liveStatus, Red);
        }
        finally
        {
            liveApplying = false;
            if (ModsPage.IsVisible) RefreshMods();
        }
    }

    void ShowLiveResult(JsonObject answer)
    {
        static List<string> Texts(JsonNode? node) => [.. (node as JsonArray ?? []).Select(t => (string?)t ?? "").Where(t => t.Length > 0)];
        var changes = Texts(answer["changes"]);
        var blocking = Texts(answer["blocking"]);
        var detail = (string?)answer["detail"] ?? "";
        if ((bool?)answer["applied"] == true)
        {
            liveStatus = "Applied to the running game: " + string.Join(", ", changes) + ".";
            Append(liveStatus + " Friends need the same mods to join.", LauncherLine);
        }
        else if (blocking.Count > 0)
        {
            liveStatus = detail + " " + string.Join(" ", blocking.Select(b => b.EndsWith('.') ? b : b + "."));
            Append("Mods: " + liveStatus, Grey);
        }
        else if (changes.Count > 0) liveStatus = detail; /* the game is not connected yet, say */
        else liveStatus = "";
    }
}
