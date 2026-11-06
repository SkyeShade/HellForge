using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;

// Synthetic connectivity control; no game services, credentials or memory access.
public static class OfflineControlNetworkProbe {
    [DllImport("iphlpapi.dll")]
    static extern int GetBestInterface(uint destination, out uint index);
    // Whether Windows has a route to the address: 0 (NO_ERROR) when it does, else GetBestInterface's error. Without one
    // the connect fails in the stack (WSAENETUNREACH, WSAEHOSTUNREACH) before WFP's connect layer ever judges it, so the
    // block's refusal (10013) cannot show: the PC has no network.
    static int Route(string address) {
        uint index;
        // IPAddr is in_addr's form (network byte order): the address bytes as they are, read as one value
        return GetBestInterface(BitConverter.ToUInt32(IPAddress.Parse(address).GetAddressBytes(), 0), out index);
    }
    // No route out (Route), and the connect failed for it: WSAENETDOWN, WSAENETUNREACH or WSAEHOSTUNREACH.
    static bool Offline(int route, int error) {
        return route != 0 && (error == 10050 || error == 10051 || error == 10065);
    }
    static int External(AddressFamily family, string address) {
        using(var socket = new Socket(family, SocketType.Stream, ProtocolType.Tcp)) {
            try {
                var task = socket.ConnectAsync(IPAddress.Parse(address), 443);
                if(!task.Wait(2500)) return -1;
                return 0;
            } catch(AggregateException error) {
                var socketError = error.GetBaseException() as SocketException;
                return socketError == null ? -2 : socketError.ErrorCode;
            } catch(SocketException error) { return error.ErrorCode; }
        }
    }
    static int Loopback(int port) {
        using(var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)) {
            try {
                var task = socket.ConnectAsync(IPAddress.Loopback, port);
                if(!task.Wait(2500)) return -1;
                return 0;
            } catch(AggregateException error) {
                var socketError = error.GetBaseException() as SocketException;
                return socketError == null ? -2 : socketError.ErrorCode;
            } catch(SocketException error) { return error.ErrorCode; }
        }
    }
    // <backend port> <report> [other loopback port]: with the third, also how a connect to the backend's port and to
    // another loopback port ends (the WFP block permits only the backend's: 10013 for the other). Offline (no route out),
    // the other port's 10013 is the proof: the WFP block's BLOCK filter names only the program, so the filter that
    // refuses it is the one that refuses the outside (native/network-block/hellforge_wfp.c, AddPair).
    public static int Main(string[] args) {
        int port, other = 0;
        if(args.Length < 2 || args.Length > 3 || !int.TryParse(args[0], out port) || port < 1 || port > 65535) return 2;
        if(args.Length == 3 && (!int.TryParse(args[2], out other) || other < 1 || other > 65535 || other == port)) return 2;
        bool local = false;
        using(var client = new TcpClient(AddressFamily.InterNetwork)) {
            try {
                var task = client.ConnectAsync(IPAddress.Loopback, port);
                if(task.Wait(2500)) {
                    client.ReceiveTimeout = 2500; client.SendTimeout = 2500;
                    using(var stream = client.GetStream()) {
                        var request = Encoding.ASCII.GetBytes("GET /config/locator.json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
                        stream.Write(request, 0, request.Length);
                        var buffer = new byte[2048];
                        int read = stream.Read(buffer, 0, buffer.Length);
                        local = Encoding.ASCII.GetString(buffer, 0, read).StartsWith("HTTP/1.1 200 ");
                    }
                }
            } catch(Exception error) {
                if(!(error is SocketException) && !(error is IOException) && !(error is AggregateException)) throw;
            }
        }
        int route = Route("192.0.2.1");
        int v4 = External(AddressFamily.InterNetwork, "192.0.2.1");
        int v6 = External(AddressFamily.InterNetworkV6, "2001:db8::1");
        string narrowing = "";
        int otherError = 0;
        if(other > 0) {
            otherError = Loopback(other);
            narrowing = ",\"loopbackTcpError\":" + Loopback(port) + ",\"otherLoopbackTcpError\":" + otherError;
        }
        File.WriteAllText(args[1], "{\"loopbackHttpPassed\":" + (local ? "true" : "false") +
            ",\"externalIpv4TcpError\":" + v4 + ",\"externalIpv4RouteError\":" + route + ",\"externalIpv6TcpError\":" + v6 + narrowing + "}");
        bool outside = v4 == 10013 || (other > 0 && Offline(route, v4));
        return local && outside && (other == 0 || otherError == 10013) ? 0 : 1;
    }
}
