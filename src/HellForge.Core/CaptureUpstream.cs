using System.Net;
using System.Net.Http;

namespace HellForge.Core;

// The capture's handler to the real API (HellForge.CaptureProxy sets its ConnectCallback, a plain TCP stream to the API's
// own addresses, and its certificate check). It never uses a proxy: the connection goes to the API's address whatever
// the handler would pick. With a Windows system proxy set (a proxy app's 127.0.0.1:7897, say), .NET took that proxy and
// sent its CONNECT through the callback to the API itself, which answered 400, and every sync failed (#346).
public static class CaptureUpstream
{
    public static SocketsHttpHandler Handler() => new()
    {
        UseProxy = false,
        AllowAutoRedirect = false,
        UseCookies = false,
        AutomaticDecompression = DecompressionMethods.All,
        ConnectTimeout = TimeSpan.FromSeconds(15)
    };
}
