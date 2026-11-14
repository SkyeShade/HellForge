using System.Text.Json.Serialization;

namespace HellForge.Core.AssetMigration;

// The asset kit's files, read and written with JSON generated at build time: a recipe arrives with a mod and is read by
// every program that loads mods, the NativeAOT session and backend among them (no reflection-based serialization there).
[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(Recipe))]
[JsonSerializable(typeof(BuildCatalog))]
partial class AssetJson : JsonSerializerContext;
