using System.Text.Json.Serialization;

namespace HellForge.Core;

// Core's embedded data, read with JSON generated at build time, so programs compiled ahead of time (NativeAOT, which
// has no reflection-based serialization) read it too: the entity profile and the stats schema as written (any case),
// the supported build and language lists in camelCase as HellForge.Cli writes it.
[JsonSourceGenerationOptions(PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(EntityProfile))]
[JsonSerializable(typeof(StatsSchema))]
partial class CoreJson : JsonSerializerContext;

[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(SupportedBuild))]
[JsonSerializable(typeof(SupportedLanguages))]
partial class CoreJsonCamel : JsonSerializerContext;
