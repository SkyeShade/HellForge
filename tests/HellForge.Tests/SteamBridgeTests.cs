using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using HellForge.Core;

// The native Steam bridge (LinuxSteamBridge): the same unix sockets, loopback ports and per-connection relay the Linux
// scripts used socat for. The upstream is a stand-in Steam listener; no Steam, Proton or namespace is needed.
static class SteamBridgeTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("Steam bridge: /proc/net/tcp listeners are picked out by address, state and socket inode", () =>
        {
            const string table = """
                  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
                   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 111 1 0000000000000000 100 0 0 10 0
                   1: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 999 1 0000000000000000 100 0 0 10 0
                   2: 0100007F:0BBC 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 111 1 0000000000000000 100 0 0 10 0
                   3: 00000000:0BBD 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 111 1 0000000000000000 100 0 0 10 0
                   4: 0100007F:0BBE 00000000:0000 01 00000000:00000000 00:00000000 00000000  1000        0 111 1 0000000000000000 100 0 0 10 0
                   5: 0100007F:0BBF 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 222 1 0000000000000000 100 0 0 10 0
                """;
            var ports = LinuxSteamBridge.LoopbackPorts(table, new HashSet<long> { 111, 222 });
            check(ports.SequenceEqual([3000, 3004, 3007]), "Wrong loopback listeners: " + string.Join(", ", ports));
            check(LinuxSteamBridge.PortOf(LinuxSteamBridge.SocketPath(root, 27036)) == 27036, "A bridge socket name did not give its port.");
            check(LinuxSteamBridge.PortOf(Path.Combine(root, "hf-steam-0.sock")) == 0, "Port 0 was accepted.");
            check(LinuxSteamBridge.PortOf(Path.Combine(root, "other-27036.sock")) == 0, "Another socket was accepted.");
            check((int)LinuxSteamBridge.SocketState.Listening == 0 &&
                (int)LinuxSteamBridge.SocketState.NotListening == 1 &&
                (int)LinuxSteamBridge.SocketState.CannotTell == 3, "The three-way check's exit codes changed.");
        });

        test("Steam bridge: says whether Steam is missing, its sockets cannot be read (steam-run), or it has no loopback ports", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux /proc layout)"); return; }
            var proc = Path.Combine(root, "fake-proc");
            const uint uid = 4242;
            string Process(int pid, string comm, uint owner, long? socket = null)
            {
                var folder = Directory.CreateDirectory(Path.Combine(proc, pid.ToString(System.Globalization.CultureInfo.InvariantCulture))).FullName;
                File.WriteAllText(Path.Combine(folder, "comm"), comm + "\n");
                File.WriteAllText(Path.Combine(folder, "status"), $"Name:\t{comm}\nUid:\t{owner}\t{owner}\t{owner}\t{owner}\n");
                var fd = Directory.CreateDirectory(Path.Combine(folder, "fd")).FullName;
                if (socket is { } inode) File.CreateSymbolicLink(Path.Combine(fd, "7"), $"socket:[{inode}]");
                File.CreateSymbolicLink(Path.Combine(fd, "1"), "/dev/null");
                return fd;
            }
            Directory.CreateDirectory(proc);
            Process(10, "bash", uid, 111);
            Process(11, "steam", uid + 1, 222);
            var none = LinuxSteamBridge.SocketsOf(proc, "steam", uid);
            check(none.Processes == 0 && none.Inodes.Count == 0 &&
                LinuxSteamBridge.NoSteamPortsReason(none).Contains("is Steam running?", StringComparison.Ordinal),
                "Another program or another user's Steam counted as this user's Steam.");
            var hidden = Process(12, "steam", uid, 333);
            File.SetUnixFileMode(hidden, UnixFileMode.None);
            try
            {
                if (Directory.EnumerateFileSystemEntries(hidden).Any()) { Console.WriteLine("  (skipped: running as root, every fd folder is readable)"); return; }
            }
            catch (UnauthorizedAccessException) { }
            // Listed but not entered (r--): the names are there, their links cannot be read.
            var listOnly = Process(14, "steam", uid, 555);
            File.SetUnixFileMode(listOnly, UnixFileMode.UserRead);
            try
            {
                var unreadable = LinuxSteamBridge.SocketsOf(proc, "steam", uid);
                check(unreadable.Processes == 2 && unreadable.Unreadable == 2 && unreadable.Inodes.Count == 0 &&
                    LinuxSteamBridge.NoSteamPortsReason(unreadable).Contains("steam-run", StringComparison.Ordinal) &&
                    LinuxSteamBridge.NoSteamPortsReason(unreadable).Contains("hf-bridge-host.sh", StringComparison.Ordinal),
                    $"Steam's unreadable sockets were not reported as another user namespace's ({unreadable.Processes} processes, " +
                    $"{unreadable.Unreadable} unreadable, {unreadable.Inodes.Count} sockets).");
                Process(13, "steam", uid, 444);
                var readable = LinuxSteamBridge.SocketsOf(proc, "steam", uid);
                check(readable.Processes == 3 && readable.Unreadable == 2 && readable.Inodes.SetEquals([444]),
                    "The readable Steam process's socket was not found: " + string.Join(", ", readable.Inodes));
                check(LinuxSteamBridge.NoSteamPortsReason(readable).Contains("No loopback TCP ports", StringComparison.Ordinal),
                    "Steam with readable sockets but no listener was reported as something else.");
            }
            finally
            {
                foreach (var fd in new[] { hidden, listOnly })
                    File.SetUnixFileMode(fd, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
        });

        test("Steam bridge: check is three-way for the wineserver guard", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux unix sockets)"); return; }
            var dir = SocketDirectory("check");
            try
            {
                var missing = Path.Combine(dir, "hf-steam-55551.sock");
                check(LinuxSteamBridge.Check(missing, out var missingProblem) == LinuxSteamBridge.SocketState.NotListening &&
                    missingProblem == null, "A missing socket did not read as definitely not listening.");
                check(SteamBridgeCommand.Run(["steam-bridge", "check", missing]) == 1,
                    "steam-bridge check did not exit 1 for a missing socket.");

                var stalePath = Path.Combine(dir, "hf-steam-55552.sock");
                using (UnixListener(stalePath)) { }
                check(LinuxSteamBridge.Check(stalePath, out _) == LinuxSteamBridge.SocketState.NotListening,
                    "A socket file with no listener did not read as definitely not listening.");
                check(SteamBridgeCommand.Run(["steam-bridge", "check", stalePath]) == 1,
                    "steam-bridge check did not exit 1 for a socket with no listener.");

                var livePath = Path.Combine(dir, "hf-steam-55553.sock");
                using (UnixListener(livePath))
                {
                    check(LinuxSteamBridge.Check(livePath, out var liveProblem) == LinuxSteamBridge.SocketState.Listening &&
                        liveProblem == null, "A listening socket did not read as listening.");
                    check(SteamBridgeCommand.Run(["steam-bridge", "check", livePath]) == 0,
                        "steam-bridge check did not exit 0 for a listening socket.");
                }

                var file = Path.Combine(dir, "not-a-socket");
                File.WriteAllText(file, "x");
                check(LinuxSteamBridge.Check(file, out var fileProblem) == LinuxSteamBridge.SocketState.CannotTell &&
                    !string.IsNullOrEmpty(fileProblem), "A regular file did not read as cannot tell.");
                check(SteamBridgeCommand.Run(["steam-bridge", "check", file]) == 3,
                    "steam-bridge check did not exit 3 for a path that is not a socket.");
            }
            finally { RemoveDirectory(dir); }
        });

        test("Steam bridge: the host relay carries bytes both ways", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux unix sockets)"); return; }
            var dir = SocketDirectory("both");
            var listener = TcpListener(out var port);
            using var serverStop = new CancellationTokenSource();
            var served = Task.Run(async () =>
            {
                using (listener)
                {
                    var client = await listener.AcceptAsync(serverStop.Token);
                    using (client)
                    {
                        await client.SendAsync("steam-hello"u8.ToArray(), SocketFlags.None, serverStop.Token);
                        var request = await ReceiveExactly(client, 10, serverStop.Token);
                        check(Encoding.UTF8.GetString(request) == "game-hello", "The upstream did not receive the client's bytes: " +
                            Encoding.UTF8.GetString(request));
                        await client.SendAsync("steam-reply"u8.ToArray(), SocketFlags.None, serverStop.Token);
                    }
                }
            });
            using var stop = new CancellationTokenSource();
            var log = new ConcurrentBag<string>();
            var host = Task.Run(() => LinuxSteamBridge.RunHost(dir, log.Add, stop.Token, [port]));
            var path = LinuxSteamBridge.SocketPath(dir, port);
            try
            {
                WaitFor(() => log.Any(line => line.Contains($"bridging Steam loopback port {port}", StringComparison.Ordinal)),
                    "The host bridge socket did not appear.");
                using var client = ConnectUnix(path);
                check(Encoding.UTF8.GetString(ReceiveBytes(client, 11)) == "steam-hello", "Steam's greeting did not reach the client.");
                SendAll(client, "game-hello"u8);
                check(Encoding.UTF8.GetString(ReceiveBytes(client, 11)) == "steam-reply", "The client's bytes did not reach Steam.");
                check(served.Wait(TimeSpan.FromSeconds(5)), "The stand-in Steam listener did not finish.");
            }
            finally { Stop(host, stop); serverStop.Cancel(); }
            check(!Directory.EnumerateFileSystemEntries(dir).Any(), "The host bridge left its socket behind.");
            RemoveDirectory(dir);
        });

        test("Steam bridge: many connections are relayed at the same time", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux unix sockets)"); return; }
            var dir = SocketDirectory("many");
            var listener = TcpListener(out var port);
            using var serverStop = new CancellationTokenSource();
            _ = Task.Run(() => AcceptLoop(listener, client => EchoConnection(client, serverStop.Token), serverStop.Token));
            using var stop = new CancellationTokenSource();
            var log = new ConcurrentBag<string>();
            var host = Task.Run(() => LinuxSteamBridge.RunHost(dir, log.Add, stop.Token, [port]));
            var path = LinuxSteamBridge.SocketPath(dir, port);
            try
            {
                WaitFor(() => log.Any(line => line.Contains($"bridging Steam loopback port {port}", StringComparison.Ordinal)),
                    "The host bridge socket did not appear.");
                var clients = Enumerable.Range(0, 24).Select(number => Task.Run(() =>
                {
                    using var client = ConnectUnix(path);
                    var payload = Encoding.UTF8.GetBytes($"connection {number} " + new string((char)('a' + number % 26), 4096));
                    SendAll(client, payload);
                    var echo = ReceiveBytes(client, payload.Length);
                    check(echo.SequenceEqual(payload), $"Connection {number} did not get its bytes back.");
                })).ToArray();
                WaitAll(clients);
            }
            finally { Stop(host, stop); serverStop.Cancel(); RemoveDirectory(dir); }
        });

        test("Steam bridge: a closed Steam port ends the relay and does not stop the bridge", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux unix sockets)"); return; }
            var dir = SocketDirectory("closed");
            var listener = TcpListener(out var port);
            using var serverStop = new CancellationTokenSource();
            var served = Task.Run(async () =>
            {
                using (listener)
                {
                    var client = await listener.AcceptAsync(serverStop.Token);
                    using (client)
                    {
                        await client.SendAsync("last-bytes"u8.ToArray(), SocketFlags.None, serverStop.Token);
                        client.Shutdown(SocketShutdown.Send);
                    }
                }
            });
            using var stop = new CancellationTokenSource();
            var log = new ConcurrentBag<string>();
            var host = Task.Run(() => LinuxSteamBridge.RunHost(dir, log.Add, stop.Token, [port]));
            var path = LinuxSteamBridge.SocketPath(dir, port);
            try
            {
                WaitFor(() => log.Any(line => line.Contains($"bridging Steam loopback port {port}", StringComparison.Ordinal)),
                    "The host bridge socket did not appear.");
                using (var client = ConnectUnix(path))
                {
                    check(Encoding.UTF8.GetString(ReceiveBytes(client, 10)) == "last-bytes", "The last bytes before Steam closed did not arrive.");
                    check(ReadToEnd(client).Length == 0, "Steam's close was not passed on to the client.");
                }
                check(served.Wait(TimeSpan.FromSeconds(5)), "The stand-in Steam listener did not finish.");
                using var late = ConnectUnix(path);
                check(ReadToEnd(late).Length == 0, "A connection to a closed Steam port was not ended.");
            }
            finally { Stop(host, stop); serverStop.Cancel(); RemoveDirectory(dir); }
        });

        test("Steam bridge: the inner relay recreates a bridge socket's loopback port", () =>
        {
            if (!OperatingSystem.IsLinux()) { Console.WriteLine("  (skipped: Linux unix sockets)"); return; }
            var dir = SocketDirectory("inner");
            var probe = TcpListener(out var port);
            probe.Dispose();
            var bridge = UnixListener(LinuxSteamBridge.SocketPath(dir, port));
            using var serverStop = new CancellationTokenSource();
            _ = Task.Run(() => AcceptLoop(bridge, client => EchoConnection(client, serverStop.Token), serverStop.Token));
            var log = new ConcurrentBag<string>();
            using var stop = new CancellationTokenSource();
            var inner = Task.Run(() => LinuxSteamBridge.RunInner(dir, log.Add, stop.Token));
            try
            {
                WaitFor(() => log.Any(line => line.Contains($"127.0.0.1:{port}", StringComparison.Ordinal)),
                    "The inner relay did not open the loopback port.");
                using var client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
                client.Connect(IPAddress.Loopback, port);
                var payload = "through the namespace and back"u8.ToArray();
                SendAll(client, payload);
                check(ReceiveBytes(client, payload.Length).SequenceEqual(payload), "The inner relay did not carry the bytes.");
            }
            finally { Stop(inner, stop); serverStop.Cancel(); RemoveDirectory(dir); }
            using var rebind = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            rebind.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            rebind.Bind(new IPEndPoint(IPAddress.Loopback, port));
        });
    }

    // Unix socket paths are limited to 108 bytes, and the test artifacts folder can be long: use a short temporary one.
    static string SocketDirectory(string name)
    {
        var path = Path.Combine(Path.GetTempPath(), "hf-sb-" + name + "-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(path);
        return path;
    }

    static void RemoveDirectory(string path)
    {
        try { Directory.Delete(path, true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    static Socket TcpListener(out int port)
    {
        var listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        listener.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        listener.Listen(64);
        port = ((IPEndPoint)listener.LocalEndPoint!).Port;
        return listener;
    }

    static Socket UnixListener(string path)
    {
        var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        listener.Bind(new UnixDomainSocketEndPoint(path));
        listener.Listen(64);
        return listener;
    }

    static Socket ConnectUnix(string path)
    {
        var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        socket.Connect(new UnixDomainSocketEndPoint(path));
        return socket;
    }

    static void WaitFor(Func<bool> condition, string reason)
    {
        var until = DateTime.UtcNow.AddSeconds(5);
        while (!condition())
        {
            if (DateTime.UtcNow >= until) throw new Exception(reason);
            Thread.Sleep(20);
        }
    }

    static void Stop(Task bridge, CancellationTokenSource stop)
    {
        stop.Cancel();
        if (!bridge.Wait(TimeSpan.FromSeconds(5))) throw new Exception("The Steam bridge did not stop when its token was cancelled.");
    }

    static void WaitAll(Task[] tasks)
    {
        try { Task.WaitAll(tasks); }
        catch (AggregateException error) { throw error.Flatten().InnerExceptions[0]; }
    }

    static async Task AcceptLoop(Socket listener, Func<Socket, Task> handle, CancellationToken stop)
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                var client = await listener.AcceptAsync(stop);
                _ = Task.Run(() => handle(client), CancellationToken.None);
            }
        }
        catch (Exception error) when (error is OperationCanceledException or ObjectDisposedException or SocketException) { }
        finally { listener.Dispose(); }
    }

    static async Task EchoConnection(Socket client, CancellationToken stop)
    {
        using (client)
        {
            var buffer = new byte[16 << 10];
            try
            {
                while (true)
                {
                    var read = await client.ReceiveAsync(buffer.AsMemory(), SocketFlags.None, stop);
                    if (read == 0)
                    {
                        try { client.Shutdown(SocketShutdown.Send); }
                        catch (Exception error) when (error is SocketException or ObjectDisposedException) { }
                        return;
                    }
                    var sent = 0;
                    while (sent < read) sent += await client.SendAsync(buffer.AsMemory(sent, read - sent), SocketFlags.None, stop);
                }
            }
            catch (Exception error) when (error is SocketException or OperationCanceledException or ObjectDisposedException) { }
        }
    }

    static async Task<byte[]> ReceiveExactly(Socket socket, int count, CancellationToken stop)
    {
        var data = new byte[count];
        var read = 0;
        while (read < count)
        {
            var got = await socket.ReceiveAsync(data.AsMemory(read, count - read), SocketFlags.None, stop);
            if (got == 0) break;
            read += got;
        }
        return data[..read];
    }

    static byte[] ReceiveBytes(Socket socket, int count)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        return ReceiveExactly(socket, count, timeout.Token).GetAwaiter().GetResult();
    }

    static byte[] ReadToEnd(Socket socket)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var buffer = new byte[4096];
        var data = new List<byte>();
        while (true)
        {
            var read = socket.ReceiveAsync(buffer.AsMemory(), SocketFlags.None, timeout.Token).AsTask().GetAwaiter().GetResult();
            if (read == 0) return data.ToArray();
            data.AddRange(buffer[..read]);
        }
    }

    static void SendAll(Socket socket, ReadOnlySpan<byte> data)
    {
        var sent = 0;
        while (sent < data.Length) sent += socket.Send(data[sent..]);
    }
}
