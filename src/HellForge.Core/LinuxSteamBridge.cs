using System.Buffers;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace HellForge.Core;

// HellForge's own Steam bridge (docs/linux.md "Sessions", .deepcode/review-E5.md). linux/hf-bridge-host.sh and
// linux/hf-inner.sh used socat to relay the Steam client's loopback TCP ports through $XDG_RUNTIME_DIR/hf-steam-*.sock
// into the session's network namespace; a stock Steam Deck has no socat. The host side listens on a unix socket for each
// loopback port the Steam client listens on; the inner side listens on the same loopback ports inside the namespace and
// connects each connection to that socket. Both sides connect only to 127.0.0.1 and to the socket directory: no name
// lookups, no other address, and no route are added.
public static class LinuxSteamBridge
{
    public const string SocketPrefix = "hf-steam-";
    public const string SocketSuffix = ".sock";
    const int ListenBacklog = 128;
    const int BufferSize = 64 << 10;
    const int AnswerTimeoutMilliseconds = 500;
    const int StatBufferSize = 256; // x86_64 Linux struct stat, enough for stat
    const int StatModeOffset = 24; // st_mode in the x86_64 Linux struct stat
    const int ENoEnt = 2;
    const int EConnRefused = 111;
    const uint FileTypeMask = 0xF000;
    const uint SocketFileType = 0xC000;

    // What `steam-bridge check <socket>` found. The values are the command's exit codes.
    public enum SocketState
    {
        Listening = 0,
        NotListening = 1,
        CannotTell = 3
    }

    // ${XDG_RUNTIME_DIR:-/run/user/<uid>}, as the scripts use.
    public static string SocketDirectory()
    {
        var runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!string.IsNullOrEmpty(runtime)) return runtime;
        return "/run/user/" + GetUid().ToString(CultureInfo.InvariantCulture);
    }

    [DllImport("libc", EntryPoint = "getuid")]
    static extern uint GetUid();

    [DllImport("libc", SetLastError = true, EntryPoint = "stat")]
    static extern int StatPath([MarshalAs(UnmanagedType.LPUTF8Str)] string path, IntPtr buffer);

    public static string SocketPath(string directory, int port) =>
        Path.Combine(directory, SocketPrefix + port.ToString(CultureInfo.InvariantCulture) + SocketSuffix);

    // The port a bridge socket's name carries, or 0 when the name is not one of ours.
    public static int PortOf(string path)
    {
        var name = Path.GetFileName(path);
        if (!name.StartsWith(SocketPrefix, StringComparison.Ordinal) || !name.EndsWith(SocketSuffix, StringComparison.Ordinal)) return 0;
        var text = name[SocketPrefix.Length..^SocketSuffix.Length];
        return int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var port) && port is > 0 and <= 65535 ? port : 0;
    }

    // The loopback (127.0.0.1) TCP listeners in /proc/net/tcp whose socket belongs to one of the given inodes. Pure: the
    // caller reads the table and finds the Steam process's socket inodes. A LISTEN entry's state is 0A; the address is
    // stored as the four bytes in host order, so 127.0.0.1 is 0100007F. This is the same set the scripts found with
    // `ss -Htlnp` and a process name of "steam".
    public static IReadOnlyList<int> LoopbackPorts(string procNetTcp, IReadOnlySet<long> socketInodes)
    {
        var ports = new SortedSet<int>();
        foreach (var line in procNetTcp.Split('\n'))
        {
            var fields = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length < 10 || fields[3] != "0A") continue;
            if (!long.TryParse(fields[9], NumberStyles.None, CultureInfo.InvariantCulture, out var inode) || !socketInodes.Contains(inode)) continue;
            var local = fields[1].Split(':');
            if (local.Length != 2 || !string.Equals(local[0], "0100007F", StringComparison.OrdinalIgnoreCase)) continue;
            if (int.TryParse(local[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var port) && port is > 0 and <= 65535)
                ports.Add(port);
        }
        return ports.ToList();
    }

    enum UnixSocketKind { Socket, Missing, Other }

    // Three-way, and the values are `steam-bridge check <socket>`'s exit codes: 0 accepted a connection, 1 is
    // definitely not listening (missing path, ENOENT or ECONNREFUSED), 3 cannot tell (timeout, EACCES, another error,
    // or a path that is not a unix socket). The session's wineserver guard must fail closed on 3.
    public static SocketState Check(string path, out string? problem)
    {
        problem = null;
        switch (SocketKind(path, out var kindProblem))
        {
            case UnixSocketKind.Socket:
                break;
            case UnixSocketKind.Missing:
                return SocketState.NotListening;
            default:
                problem = kindProblem ?? $"{path} is not a unix socket";
                return SocketState.CannotTell;
        }
        try
        {
            using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            using var timeout = new CancellationTokenSource(AnswerTimeoutMilliseconds);
            socket.ConnectAsync(new UnixDomainSocketEndPoint(path), timeout.Token).AsTask().GetAwaiter().GetResult();
            return SocketState.Listening;
        }
        catch (SocketException error) when (DefinitelyNotListening(error))
        {
            return SocketState.NotListening;
        }
        catch (SocketException error)
        {
            problem = $"the socket could not be checked: {error.Message} (errno {error.ErrorCode})";
            return SocketState.CannotTell;
        }
        catch (OperationCanceledException)
        {
            problem = $"the socket did not answer within {AnswerTimeoutMilliseconds} ms";
            return SocketState.CannotTell;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            problem = $"the socket could not be checked: {error.Message}";
            return SocketState.CannotTell;
        }
    }

    // True when something accepts a connection on the unix socket, as the scripts' `socat -u OPEN:/dev/null
    // UNIX-CONNECT:<path>` did. A stale entry left by a killed bridge does not answer and is removed by the next host.
    public static bool Answers(string path) => Check(path, out _) == SocketState.Listening;

    static bool DefinitelyNotListening(SocketException error) =>
        error.SocketErrorCode == SocketError.ConnectionRefused || error.ErrorCode is ENoEnt or EConnRefused;

    // The bridge is Linux x86_64 (the package architecture); elsewhere this cannot tell, which callers must fail closed.
    // x86_64 Linux struct stat puts st_mode at offset 24. This is only used to refuse a regular file or directory posing
    // as a socket: connect(2) returns ECONNREFUSED for those too. stat follows symlinks, like the old `test -S`.
    static UnixSocketKind SocketKind(string path, out string? problem)
    {
        problem = null;
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64)
        {
            problem = "the socket file type check is implemented for x86_64 Linux only";
            return UnixSocketKind.Other;
        }
        nint buffer = Marshal.AllocHGlobal(StatBufferSize);
        try
        {
            Marshal.Copy(new byte[StatBufferSize], 0, buffer, StatBufferSize);
            if (StatPath(path, buffer) != 0)
            {
                int error = Marshal.GetLastPInvokeError();
                if (error == ENoEnt) return UnixSocketKind.Missing;
                problem = $"the socket path could not be read (errno {error})";
                return UnixSocketKind.Other;
            }
            uint mode = (uint)Marshal.ReadInt32(buffer, StatModeOffset);
            if ((mode & FileTypeMask) == SocketFileType) return UnixSocketKind.Socket;
            problem = $"{path} is not a unix socket";
            return UnixSocketKind.Other;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static bool AnyAnswers(string directory) => SocketEntries(directory).Any(Answers);

    // Host side: linux/hf-bridge-host.sh execs this. ports is supplied by tests; null means the ports the Steam client
    // itself listens on. The sockets it creates are removed on return; ones another live bridge owns are left alone.
    public static void RunHost(string directory, Action<string> log, CancellationToken stop, IReadOnlyCollection<int>? ports = null)
    {
        if (!Directory.Exists(directory))
            throw new InvalidDataException("The Steam bridge's socket folder is missing: " + directory);
        var listeners = new List<Socket>();
        var created = new List<string>();
        var already = 0;
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(stop);
        try
        {
            RemoveStaleSockets(directory);
            string? reason = null;
            var wanted = (ports ?? SteamPorts(out reason)).Distinct().Order().ToArray();
            if (wanted.Length == 0)
                throw new InvalidDataException(reason ?? "No loopback TCP ports were found for the Steam process; is Steam running?");
            foreach (var port in wanted)
            {
                var path = SocketPath(directory, port);
                var state = Check(path, out _);
                if (state == SocketState.Listening)
                {
                    already++;
                    log($"{path} already answers; leaving the running bridge alone.");
                    continue;
                }
                if (state == SocketState.NotListening) TryDelete(path);
                Socket listener;
                try { listener = ListenUnix(path); }
                catch (Exception error) when (error is SocketException or IOException or UnauthorizedAccessException or ArgumentException)
                {
                    log($"Could not bridge Steam loopback port {port}: {error.Message}");
                    continue;
                }
                listeners.Add(listener);
                created.Add(path);
                _ = AcceptLoop(listener, (client, token) => RelayTcp(client, port, token), log, linked.Token);
                log($"bridging Steam loopback port {port}");
            }
            if (created.Count == 0)
            {
                if (already == 0) throw new IOException("None of Steam's loopback ports could be bridged.");
                log("The Steam bridge is already running.");
                return;
            }
            log("Bridge running. Leave this open; Ctrl-C stops it.");
            stop.WaitHandle.WaitOne();
        }
        finally
        {
            linked.Cancel();
            foreach (var listener in listeners) listener.Dispose();
            foreach (var path in created) TryDelete(path);
        }
    }

    // Inner side: linux/hf-inner.sh starts this inside the network namespace. Each bridge socket's port is opened on
    // 127.0.0.1 there, and each connection is relayed to the unix socket outside.
    public static void RunInner(string directory, Action<string> log, CancellationToken stop)
    {
        if (!Directory.Exists(directory))
            throw new InvalidDataException("The Steam bridge's socket folder is missing: " + directory);
        var sockets = SocketEntries(directory)
            .Select(path => (Port: PortOf(path), Path: path))
            .Where(socket => socket.Port > 0)
            .OrderBy(socket => socket.Port)
            .ToArray();
        if (sockets.Length == 0)
            throw new InvalidDataException("No bridge sockets were found; start hf-bridge-host.sh first.");
        var listeners = new List<Socket>();
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(stop);
        try
        {
            foreach (var (port, path) in sockets)
            {
                Socket listener;
                try { listener = ListenTcp(port); }
                catch (Exception error) when (error is SocketException or IOException or UnauthorizedAccessException)
                {
                    log($"Could not listen on 127.0.0.1:{port}: {error.Message}");
                    continue;
                }
                listeners.Add(listener);
                var socketPath = path;
                _ = AcceptLoop(listener, (client, token) => RelayUnix(client, socketPath, token), log, linked.Token);
                log($"forwarding 127.0.0.1:{port} -> Steam");
            }
            if (listeners.Count == 0) throw new IOException("None of the Steam bridge's loopback ports could be opened.");
            stop.WaitHandle.WaitOne();
        }
        finally
        {
            linked.Cancel();
            foreach (var listener in listeners) listener.Dispose();
        }
    }

    // Steam's own loopback listeners: the socket inodes of this user's processes named "steam", matched against
    // /proc/net/tcp. With none, reason says why.
    static IReadOnlyList<int> SteamPorts(out string reason)
    {
        var steam = SocketsOf("/proc", "steam", GetUid());
        reason = NoSteamPortsReason(steam);
        if (steam.Inodes.Count == 0) return [];
        try { return LoopbackPorts(File.ReadAllText("/proc/net/tcp"), steam.Inodes); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    // The socket inodes of the processes named processName that run as uid, how many such processes there are, and how
    // many of them this program may not read the open files of. A running process always has some, so none readable
    // means /proc/<pid>/fd could not be listed or its links could not be read (the listing also leaves out entries it
    // cannot look at).
    public sealed record ProcessSockets(IReadOnlySet<long> Inodes, int Processes, int Unreadable);

    public static ProcessSockets SocketsOf(string procRoot, string processName, uint uid)
    {
        var inodes = new HashSet<long>();
        int processes = 0, unreadable = 0;
        foreach (var processFolder in Entries(procRoot))
        {
            if (!uint.TryParse(Path.GetFileName(processFolder), NumberStyles.None, CultureInfo.InvariantCulture, out _)) continue;
            if (!IsNamed(processFolder, processName) || RealUid(processFolder) != uid) continue;
            processes++;
            if (AddSocketLinks(Path.Combine(processFolder, "fd"), inodes) == 0) unreadable++;
        }
        return new(inodes, processes, unreadable);
    }

    // Adds the socket inodes among one process's open files. The number of its fd links that could be read; -1 when the
    // process is gone.
    static int AddSocketLinks(string fdFolder, HashSet<long> inodes)
    {
        string[] entries;
        try { entries = Directory.EnumerateFileSystemEntries(fdFolder).ToArray(); }
        catch (UnauthorizedAccessException) { return 0; }
        catch (IOException) { return -1; }
        var readable = 0;
        foreach (var entry in entries)
        {
            string? target;
            try { target = new FileInfo(entry).LinkTarget; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { continue; }
            // Every fd entry is a link: .NET gives null for one it can list but not read (measured: a folder without x).
            if (target is null) continue;
            readable++;
            if (SocketInode(target) is { } inode) inodes.Add(inode);
        }
        return readable;
    }

    // Why no Steam port was found. Measured on NixOS (#112): inside steam-run, Steam's process is visible in the same PID
    // namespace, but the sandbox has its own user namespace and Steam's /proc/<pid>/fd cannot be read from it; the same
    // listing outside steam-run shows Steam's sockets.
    public static string NoSteamPortsReason(ProcessSockets steam) =>
        steam.Processes == 0 ? "No Steam client process (named steam) of this user is visible here; is Steam running?"
        : steam.Inodes.Count == 0 && steam.Unreadable > 0
            ? "Steam is running, but this program may not read its open sockets (/proc/<pid>/fd): it runs in another user namespace " +
              "than Steam, for example inside NixOS's steam-run or another sandbox. Start app/linux/hf-bridge-host.sh where Steam runs " +
              "(for example as a Steam shortcut) and leave it running; HellForge then uses that bridge."
            : "No loopback TCP ports were found for the Steam process; is Steam running and signed in?";

    // The real user id in /proc/<pid>/status, which every process may read; null when the process is gone.
    static uint? RealUid(string processFolder)
    {
        try
        {
            foreach (var line in File.ReadLines(Path.Combine(processFolder, "status")))
                if (line.StartsWith("Uid:", StringComparison.Ordinal))
                    return line[4..].Split(['\t', ' '], StringSplitOptions.RemoveEmptyEntries) is [var real, ..] &&
                        uint.TryParse(real, NumberStyles.None, CultureInfo.InvariantCulture, out var uid) ? uid : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return null;
    }

    static bool IsNamed(string processFolder, string processName)
    {
        try { return File.ReadAllText(Path.Combine(processFolder, "comm")).Trim() == processName; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    static long? SocketInode(string? target)
    {
        if (target is null || target.Length <= 8 || !target.StartsWith("socket:[", StringComparison.Ordinal) || !target.EndsWith(']')) return null;
        return long.TryParse(target[8..^1], NumberStyles.None, CultureInfo.InvariantCulture, out var inode) ? inode : null;
    }

    static IReadOnlyList<string> SocketEntries(string directory) =>
        Entries(directory, SocketPrefix + "*" + SocketSuffix);

    static IReadOnlyList<string> Entries(string directory, string? pattern = null)
    {
        try
        {
            return (pattern == null ? Directory.EnumerateFileSystemEntries(directory) : Directory.EnumerateFileSystemEntries(directory, pattern)).ToArray();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    static Socket ListenUnix(string path)
    {
        if (!OperatingSystem.IsLinux()) throw new PlatformNotSupportedException("The Steam bridge's unix sockets are Linux only.");
        var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        var bound = false;
        try
        {
            listener.Bind(new UnixDomainSocketEndPoint(path));
            bound = true;
            // Same as socat's mode=600: only this account may connect.
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            listener.Listen(ListenBacklog);
            return listener;
        }
        catch
        {
            listener.Dispose();
            if (bound) TryDelete(path);
            throw;
        }
    }

    static Socket ListenTcp(int port)
    {
        var listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        try
        {
            listener.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            listener.Bind(new IPEndPoint(IPAddress.Loopback, port));
            listener.Listen(ListenBacklog);
            return listener;
        }
        catch
        {
            listener.Dispose();
            throw;
        }
    }

    static async Task AcceptLoop(Socket listener, Func<Socket, CancellationToken, Task> handle, Action<string> log, CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            Socket client;
            try { client = await listener.AcceptAsync(stop); }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException error)
            {
                if (stop.IsCancellationRequested) return;
                log("A Steam bridge connection could not be accepted: " + error.Message);
                continue;
            }
            _ = Task.Run(async () =>
            {
                try { await handle(client, stop); }
                catch (Exception error) when (error is SocketException or IOException or OperationCanceledException or ObjectDisposedException) { }
                finally { client.Dispose(); }
            }, CancellationToken.None);
        }
    }

    static async Task RelayTcp(Socket client, int port, CancellationToken stop)
    {
        using (client)
        using (var upstream = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
        {
            upstream.NoDelay = true;
            try { await upstream.ConnectAsync(IPAddress.Loopback, port, stop); }
            catch (Exception error) when (error is SocketException or OperationCanceledException or ObjectDisposedException) { return; }
            await Relay(client, upstream, stop);
        }
    }

    static async Task RelayUnix(Socket client, string path, CancellationToken stop)
    {
        using (client)
        using (var upstream = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified))
        {
            client.NoDelay = true;
            try { await upstream.ConnectAsync(new UnixDomainSocketEndPoint(path), stop); }
            catch (Exception error) when (error is SocketException or OperationCanceledException or ObjectDisposedException or ArgumentException) { return; }
            await Relay(client, upstream, stop);
        }
    }

    // Both directions, until both have seen the end of their stream (or one fails, which cancels the other).
    static async Task Relay(Socket left, Socket right, CancellationToken stop)
    {
        using var done = CancellationTokenSource.CreateLinkedTokenSource(stop);
        await Task.WhenAll(Pump(left, right, done), Pump(right, left, done));
    }

    static async Task Pump(Socket from, Socket to, CancellationTokenSource done)
    {
        var buffer = ArrayPool<byte>.Shared.Rent(BufferSize);
        try
        {
            while (true)
            {
                int read;
                try { read = await from.ReceiveAsync(buffer.AsMemory(), SocketFlags.None, done.Token); }
                catch (Exception error) when (error is SocketException or OperationCanceledException or ObjectDisposedException)
                {
                    Cancel(done);
                    return;
                }
                if (read == 0)
                {
                    try { to.Shutdown(SocketShutdown.Send); }
                    catch (Exception error) when (error is SocketException or ObjectDisposedException) { }
                    return;
                }
                try { await SendAll(to, buffer.AsMemory(0, read), done.Token); }
                catch (Exception error) when (error is SocketException or OperationCanceledException or ObjectDisposedException)
                {
                    Cancel(done);
                    return;
                }
            }
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }

    static async Task SendAll(Socket socket, ReadOnlyMemory<byte> data, CancellationToken stop)
    {
        var sent = 0;
        while (sent < data.Length) sent += await socket.SendAsync(data.Slice(sent), SocketFlags.None, stop);
    }

    static void Cancel(CancellationTokenSource source)
    {
        try { source.Cancel(); }
        catch (ObjectDisposedException) { }
    }

    static void RemoveStaleSockets(string directory)
    {
        foreach (var path in SocketEntries(directory))
            if (Check(path, out _) == SocketState.NotListening) TryDelete(path);
    }

    static void TryDelete(string path)
    {
        try { File.Delete(path); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }
}
