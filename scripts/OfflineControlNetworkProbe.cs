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
    // Windows' Network List Manager (netlistmgr.h). INetworkListManager derives from IDispatch; GetConnectivity is its
    // seventh method. The six before it are declared only to hold their places in the table and are never called.
    [ComImport, Guid("DCB00000-570F-4A9B-8D69-199FDBA5723B"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
    interface INetworkListManager {
        [PreserveSig] int GetNetworks(int flags, out IntPtr networks);
        [PreserveSig] int GetNetwork(Guid id, out IntPtr network);
        [PreserveSig] int GetNetworkConnections(out IntPtr connections);
        [PreserveSig] int GetNetworkConnection(Guid id, out IntPtr connection);
        [PreserveSig] int IsConnectedToInternet(out short connected);
        [PreserveSig] int IsConnected(out short connected);
        [PreserveSig] int GetConnectivity(out int connectivity);
    }
    [ComImport, Guid("DCB00C01-570F-4A9B-8D69-199FDBA5723B")]
    class NetworkListManager { }
    // NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET
    const int Internet = 0x40 | 0x400;
    // Windows' own verdict on this PC's connectivity (the taskbar's "No internet"): the NLM_CONNECTIVITY flags of
    // INetworkListManager::GetConnectivity, or -1 when they could not be read (which never counts as offline).
    static int Connectivity() {
        object manager = null;
        try {
            manager = new NetworkListManager();
            int flags;
            return ((INetworkListManager)manager).GetConnectivity(out flags) == 0 ? flags : -1;
        } catch(Exception) {
            return -1;
        } finally {
            if(manager != null) Marshal.ReleaseComObject(manager);
        }
    }
    // The outside connect failed in the stack (WSAENETDOWN, WSAENETUNREACH or WSAEHOSTUNREACH; the WFP block's refusal is
    // always WSAEACCES) on a PC that is offline: no route out, or a route (a network with no way on, ticket 0352) while
    // Windows sees no internet on either protocol. A route with the internet up fails: a router's unreachable answer
    // means the connect got out.
    static bool Offline(int route, int connectivity, int error) {
        if(error != 10050 && error != 10051 && error != 10065) return false;
        return route != 0 || (connectivity >= 0 && (connectivity & Internet) == 0);
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
    // another loopback port ends (the WFP block permits only the backend's: 10013 for the other). Offline (Offline above),
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
        int connectivity = Connectivity();
        int v4 = External(AddressFamily.InterNetwork, "192.0.2.1");
        int v6 = External(AddressFamily.InterNetworkV6, "2001:db8::1");
        string narrowing = "";
        int otherError = 0;
        if(other > 0) {
            otherError = Loopback(other);
            narrowing = ",\"loopbackTcpError\":" + Loopback(port) + ",\"otherLoopbackTcpError\":" + otherError;
        }
        File.WriteAllText(args[1], "{\"loopbackHttpPassed\":" + (local ? "true" : "false") +
            ",\"externalIpv4TcpError\":" + v4 + ",\"externalIpv4RouteError\":" + route + ",\"internetConnectivity\":" + connectivity + ",\"externalIpv6TcpError\":" + v6 + narrowing + "}");
        bool outside = v4 == 10013 || (other > 0 && Offline(route, connectivity, v4));
        return local && outside && (other == 0 || otherError == 10013) ? 0 : 1;
    }
}
