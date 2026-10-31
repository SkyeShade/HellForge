using System.Text.Json.Serialization;

namespace HellForge.Core;

// The sandbox request, launch spec, readiness record and evidence files are written by NativeAOT session/copy-back paths.
// Generate their metadata at build time with the same camelCase/indented shape CopyPreparer.JsonOptions uses for the
// other files.
[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(SandboxStartRequest))]
[JsonSerializable(typeof(SandboxLaunchSpec))]
[JsonSerializable(typeof(SandboxDomainStatus))]
[JsonSerializable(typeof(SandboxWindowSample))]
[JsonSerializable(typeof(SandboxCopyBackReceipt))]
[JsonSerializable(typeof(LinuxSandboxSelfTest))]
[JsonSerializable(typeof(LinuxSandboxSelfTestMode))]
public partial class SandboxJson : JsonSerializerContext;
