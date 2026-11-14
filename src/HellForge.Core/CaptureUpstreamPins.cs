using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace HellForge.Core;

// A signing capture keeps only what Arrowhead's API itself answered, so its connection there must reach the API: the
// certificate must validate for the API's name, and its chain must hold one of these keys. A CA the player's account can
// add to a trust store then cannot stand in. The pins are Let's Encrypt's roots (SHA-256 of the SubjectPublicKeyInfo,
// base64), which outlive its intermediates; the scheduled workflow upstream-pins.yml checks them against the live chain.
public static class CaptureUpstreamPins
{
    public const string ApiHost = "api.live.prod.thehelldiversgame.com";

    public static readonly IReadOnlyList<string> Pins =
    [
        "C5+lpZ7tcVwmwQIMcRtPbsQtWLABXhQzejna0wHFr8M=", // ISRG Root X1
        "diGVwiVYbubAI3RW4hB9xU8e/CH2GnkuvVFZE8zmgzI=", // ISRG Root X2
        "fk6IOKit1ild5647BH06ujSIq5XbCgqlbYl6ANhhi88=", // ISRG Root YR
        "sCkq5UWXjg+7mKu9lMhhYF5bGLsy7VI/UNW3tccdR7w=" // ISRG Root YE
    ];

    public static string Pin(X509Certificate2 certificate) =>
        Convert.ToBase64String(SHA256.HashData(certificate.PublicKey.ExportSubjectPublicKeyInfo()));

    // Null when the connection may carry a capture to sign; otherwise why not.
    public static string? Refusal(X509Chain? chain, SslPolicyErrors errors)
    {
        if (errors != SslPolicyErrors.None) return "the API's certificate does not validate (" + errors + ")";
        if (chain == null || chain.ChainElements.Count == 0) return "the API sent no certificate chain";
        foreach (var element in chain.ChainElements)
            if (Pins.Contains(Pin(element.Certificate))) return null;
        return "the API's certificate chain holds none of HellForge's pinned keys";
    }
}
