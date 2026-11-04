using HellForge.Core;

namespace HellForge.LocalBackend.Multiplayer;

// Co-op over Steam relays on Linux (docs/linux.md "Co-op on Linux"). Steam's networking, which every join and co-op
// game uses, runs in this process and reaches Valve's relays over UDP; a Linux session runs inside a network namespace
// (HELLFORGE_LINUX_NETNS=1) that has a route to them only while the setting is on (HELLFORGE_STEAM_UDP=1). Off, or when
// it could not open (HELLFORGE_STEAM_UDP_PROBLEM says why), lobbies are found but no join can reach a host: the lobby
// browser lists them with the fix (JoinCheck.FixRelays), this machine's lobby is not listed, and the game's own joins
// get a notice. TurnOnAsync asks the launcher, outside the namespace, to open it while the session runs
// (CoopRelaysSwitch). Elsewhere (Windows), always open.
public sealed class CoopRelays
{
    // How long the launcher may take: Valve's relay list (up to 20 s), pasta, the network check (up to 9 s).
    public static readonly TimeSpan AnswerTimeout = TimeSpan.FromSeconds(60);

    readonly object gate = new();
    readonly bool namespaced;
    readonly string? folder;
    readonly Action<string> log;
    bool open;
    string? problem;
    Task<string?>? turning;

    public CoopRelays(bool namespaced, bool open, string? problem, string? folder, Action<string> log)
    {
        (this.namespaced, this.open, this.problem, this.folder, this.log) = (namespaced, open, string.IsNullOrWhiteSpace(problem) ? null : problem.Trim(), folder, log);
    }

    // This session's: the namespace and the switch as the wrapper passed them; the asks go to its backend folder.
    public static CoopRelays FromEnvironment(string evidenceDirectory, Action<string> log) =>
        new(Environment.GetEnvironmentVariable("HELLFORGE_LINUX_NETNS") == "1", Environment.GetEnvironmentVariable("HELLFORGE_STEAM_UDP") == "1",
            Environment.GetEnvironmentVariable("HELLFORGE_STEAM_UDP_PROBLEM"), evidenceDirectory, log);

    // Whether a join can reach another machine.
    public bool Open
    {
        get { lock (gate) return !namespaced || open; }
    }

    // Raised once the launcher opened it while the session runs.
    public event Action? Opened;

    // Why no lobby can be joined, as a join check's Why (the browser starts it with a capital): the setting is off, or it
    // could not open for this session, and why.
    public string Why
    {
        get
        {
            lock (gate)
                return problem == null ? "co-op over Steam relays is off (the launcher's Settings > MULTIPLAYER)"
                    : "co-op over Steam relays could not open for this session: " + problem;
        }
    }

    // A join check as it stands while closed: a lobby that could be joined, or that a setting of the player's would let
    // in, can be joined once this opens (JoinCheck.FixRelays); one with mods in the way keeps its own reason.
    public JoinCheck Check(JoinCheck join)
    {
        if (Open || !join.Joinable && join.Fix == null) return join;
        return join with { Joinable = false, Why = Why, Fix = JoinCheck.FixRelays };
    }

    // The first lines of the session's multiplayer log: whether joins can reach anyone.
    public void LogState()
    {
        if (!namespaced) return;
        log(Open ? "Co-op over Steam relays: on (UDP to Valve's relays only); joins and this machine's lobby reach other players."
            : $"Co-op over Steam relays: off for this session ({Why}). Lobbies are found, but none can be joined and this machine's lobby is not listed; the lobby browser offers to turn it on.");
    }

    // The lobby browser's ask: the launcher opens it while the session runs. Null once open, else why not. One ask at a
    // time: a second waits for the first's answer.
    public Task<string?> TurnOnAsync()
    {
        lock (gate)
        {
            if (!namespaced || open) return Task.FromResult<string?>(null);
            if (turning is { IsCompleted: false } running) return running;
            return turning = AskAsync();
        }
    }

    async Task<string?> AskAsync()
    {
        if (folder == null) return "this session has no backend folder to ask the launcher through";
        log("Co-op over Steam relays: the player asked to turn it on (the lobby browser); asking the launcher.");
        try { CoopRelaysSwitch.Ask(folder); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            log("Co-op over Steam relays: could not ask the launcher: " + error.Message);
            return "the launcher could not be asked: " + error.Message;
        }
        var until = DateTime.UtcNow + AnswerTimeout;
        while (DateTime.UtcNow < until)
        {
            await Task.Delay(250);
            if (CoopRelaysSwitch.TakeAnswer(folder) is not { } answer) continue;
            if (!answer.Open)
            {
                log("Co-op over Steam relays: the launcher could not turn it on: " + answer.Message);
                return answer.Message.Length > 0 ? answer.Message : "the launcher could not turn it on";
            }
            lock (gate) (open, problem) = (true, null);
            log("Co-op over Steam relays: on for the rest of this session (the launcher opened UDP to Valve's relays only); joins and this machine's lobby reach other players now.");
            Opened?.Invoke();
            return null;
        }
        log("Co-op over Steam relays: no answer from the launcher in " + (int)AnswerTimeout.TotalSeconds + " s.");
        return "HellForge's launcher did not answer (was this session started from it?). Turn on Settings > MULTIPLAYER > Co-op over Steam relays, then start again";
    }
}
