using System.Text;
using System.Text.RegularExpressions;
using HellForge.SdkShimGen;

// Generates the HellForge stand-ins for the PlayFab SDK DLLs (native/sdk-shims) from the original DLL's export table and
// Microsoft's public SDK headers. Output goes to a build directory and is never committed: it repeats the headers'
// signatures. Functions defined in the hand-written source are skipped; every other export gets a logged stub whose
// default result is success when it has no output parameters and a failure otherwise. Like the real libraries, a stub
// clears its write-only outputs either way, so the game never reads a value nobody wrote.
const string Usage = """
    HellForge.SdkShimGen generate <prefix> <original.dll> <handwritten.cpp> <output directory> <header>...
    HellForge.SdkShimGen verify <original.dll> <stand-in.dll>
    HellForge.SdkShimGen smoke <directory with both stand-ins>
    HellForge.SdkShimGen forward <original.dll> <real module> <handwritten.c> <output.def>
    HellForge.SdkShimGen census-thunks <original.dll> <real module> <handwritten.c> <output.def> <output.c>   (the Steam census's stand-in)
    HellForge.SdkShimGen steam-bridge <steam_api.json> <original steam_api64.dll> <bridge-methods.json> <handwritten.c[,more.c]> <output directory>   (the Steam bridge)
    HellForge.SdkShimGen verify-forward <original.dll> <real module> <handwritten.c> <stand-in.dll>
    HellForge.SdkShimGen smoke-cloud <steam_api64 stand-in> <original steam_api64.dll>
    HellForge.SdkShimGen smoke-lobby-link <directory with the stand-ins> <HellForge.LocalBackend.dll>
    HellForge.SdkShimGen smoke-lobby-search <directory with PlayFabMultiplayerWin.dll> <HellForge.LocalBackend.dll>
    HellForge.SdkShimGen smoke-modloader <directory with the stand-ins and loader> <directory with the test DLLs> <Machine Gun 10k code mod>
    """;
try
{
    return args switch
    {
        ["generate", var prefix, var original, var handwritten, var output, .. var headers] when headers.Length > 0 =>
            Generate(prefix, original, handwritten, output, headers),
        ["verify", var original, var standIn] => Verify(original, standIn),
        ["smoke", var directory] => Smoke.Run(directory),
        ["forward", var original, var module, var handwritten, var output] => Forward(original, module, handwritten, output),
        ["census-thunks", var original, var module, var handwritten, var definition, var code] =>
            CensusThunks.Write(original, module, handwritten, definition, code, OwnFunctions),
        ["verify-forward", var original, var module, var handwritten, var standIn] => VerifyForward(original, module, handwritten, standIn),
        ["steam-bridge", var sdkJson, var original, var table, var handwritten, var output] =>
            SteamBridge.Write(sdkJson, original, table, handwritten, output,
                files => files.Split(',').SelectMany(OwnFunctions).ToHashSet(StringComparer.Ordinal)),
        ["smoke-cloud", var standIn, var original] => CloudSmoke.Run(standIn, original),
        ["smoke-lobby-link", var directory, var backend] => LobbyLinkSmoke.Run(directory, backend),
        ["smoke-lobby-search", var searchDirectory, var searchBackend] => LobbySearchSmoke.Run(searchDirectory, searchBackend),
        ["smoke-modloader", var directory, var tests, var machineGun] => ModLoaderSmoke.Run(directory, tests, machineGun),
        ["smoke-modloader", var directory, var tests, var machineGun, var compendium] => ModLoaderSmoke.Run(directory, tests, machineGun, compendium),
        _ => Fail(Usage)
    };
}
catch (Exception error) when (error is IOException or FormatException or InvalidDataException or BadImageFormatException)
{
    return Fail(error.Message);
}

static int Generate(string prefix, string original, string handwritten, string output, string[] headers)
{
    if (!Regex.IsMatch(prefix, "^[a-z]+$")) return Fail("Prefix must be lowercase letters.");
    var exports = PeTables.Exports(original).OrderBy(e => e.Name, StringComparer.Ordinal).ToList();
    var declared = new Dictionary<string, HeaderFunction>(StringComparer.Ordinal);
    foreach (var header in headers)
        foreach (var function in HeaderFunctions.Parse(File.ReadAllText(header)))
            declared.TryAdd(function.Name, function);
    var handwrittenText = File.ReadAllText(handwritten);
    var implemented = Regex.Matches(handwrittenText, @"^[\w \t\*]+?\b(?:PARTY_API|PFMULTIPLAYER_API)\s+(\w+)\s*\(", RegexOptions.Multiline)
        .Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);
    // Exports that no public header declares are defined by hand as HELLFORGE_UNDECLARED(name): exported, no arguments read.
    implemented.UnionWith(Regex.Matches(handwrittenText, @"^HELLFORGE_UNDECLARED\((\w+)\)", RegexOptions.Multiline).Select(m => m.Groups[1].Value));
    var missing = exports.Where(e => !declared.ContainsKey(e.Name) && !implemented.Contains(e.Name)).Select(e => e.Name).ToList();
    if (missing.Count > 0) return Fail("Exports without a header declaration: " + string.Join(", ", missing));
    var exportNames = exports.Select(e => e.Name).ToHashSet(StringComparer.Ordinal);
    var unknown = implemented.Where(name => !exportNames.Contains(name)).ToList();
    if (unknown.Count > 0) return Fail("Hand-written functions the original does not export: " + string.Join(", ", unknown));

    Directory.CreateDirectory(output);
    var dllName = Path.GetFileName(original);
    var banner = $"// Generated by HellForge.SdkShimGen from {dllName} exports and the PlayFab SDK headers. Do not edit or commit.";

    var index = new StringBuilder().AppendLine(banner).AppendLine("#pragma once").AppendLine("#include <stdint.h>")
        .AppendLine("namespace hellforge::fn {");
    for (var i = 0; i < exports.Count; i++) index.AppendLine($"constexpr uint32_t {exports[i].Name} = {i};");
    index.AppendLine("}");
    File.WriteAllText(Path.Combine(output, prefix + "_index.g.h"), index.ToString());

    var source = new StringBuilder().AppendLine(banner).AppendLine("#include \"shim_common.h\"");
    foreach (var header in headers) source.AppendLine($"#include <{Path.GetFileName(header)}>");
    source.AppendLine($"#include \"{prefix}_index.g.h\"").AppendLine()
        .AppendLine("namespace hellforge {")
        .AppendLine($"const char g_moduleName[] = \"{dllName}\";")
        .AppendLine($"const uint32_t g_functionCount = {exports.Count};")
        .AppendLine("const FunctionInfo g_functions[] = {");
    foreach (var export in exports)
        source.AppendLine($"    {{\"{export.Name}\", {(HasOutput(export.Name) ? "true" : "false")}}},");
    source.AppendLine("};").AppendLine("}").AppendLine();
    var stubs = 0;
    foreach (var export in exports.Where(e => !implemented.Contains(e.Name)))
    {
        var function = declared[export.Name];
        var (ok, fail) = function.ReturnType switch
        {
            "PartyError" => ("c_partyErrorSuccess", "c_partyErrorFailedLoadingPartyModule"),
            "HRESULT" => ("S_OK", "E_NOTIMPL"),
            _ => throw new FormatException($"{function.Name} returns {function.ReturnType}; implement it by hand.")
        };
        var arguments = string.Concat(function.Parameters.Select(p => ", " + p.Name));
        source.AppendLine($"{function.ReturnType} {function.CallingConvention} {function.Name}({string.Join(", ", function.Parameters.Select(p => p.Text))}){(function.Suffix.Length > 0 ? " " + function.Suffix : "")}")
            .AppendLine("{");
        foreach (var written in function.Parameters.Where(p => p.WriteOnly)) source.AppendLine($"    hellforge::Clear({written.Name});");
        source.AppendLine($"    return hellforge::Generic<{function.ReturnType}>(hellforge::fn::{function.Name}, {ok}, {fail}{arguments});")
            .AppendLine("}").AppendLine();
        stubs++;
    }
    File.WriteAllText(Path.Combine(output, prefix + "_gen.g.cpp"), source.ToString());

    var definition = new StringBuilder().AppendLine($"LIBRARY {dllName}").AppendLine("EXPORTS");
    foreach (var export in exports.OrderBy(e => e.Ordinal)) definition.AppendLine($"    {export.Name} @{export.Ordinal}");
    File.WriteAllText(Path.Combine(output, Path.GetFileNameWithoutExtension(dllName) + ".def"), definition.ToString());

    Console.WriteLine($"{dllName}: {exports.Count} exports, {implemented.Count} hand-written, {stubs} generated stubs " +
        $"({exports.Count(e => !implemented.Contains(e.Name) && !HasOutput(e.Name))} succeed by default).");
    return 0;

    bool HasOutput(string name) => declared.TryGetValue(name, out var function) && function.Parameters.Any(p => p.IsOutput);
}

// The stand-in must export exactly the original's names and ordinals and depend only on Windows system modules.
static int Verify(string original, string standIn)
{
    var expected = PeTables.Exports(original).Select(e => $"{e.Name} @{e.Ordinal}").ToHashSet(StringComparer.Ordinal);
    var actual = PeTables.Exports(standIn).Select(e => $"{e.Name} @{e.Ordinal}").ToHashSet(StringComparer.Ordinal);
    var problems = expected.Except(actual).Select(e => "missing export " + e)
        .Concat(actual.Except(expected).Select(e => "extra export " + e)).ToList();
    var modules = PeTables.ImportedModules(standIn);
    problems.AddRange(modules.Where(m => !IsSystemModule(m)).Select(m => "non-system import " + m));
    Console.WriteLine($"{Path.GetFileName(standIn)}: {actual.Count}/{expected.Count} exports; imports {string.Join(", ", modules)}");
    foreach (var problem in problems) Console.Error.WriteLine("  " + problem);
    return problems.Count == 0 ? 0 : 1;
}

// Functions the hand-written source of a forwarding stand-in defines itself (marked HELLFORGE_API); every other export of
// the original is forwarded to the real DLL under the same name and ordinal.
static HashSet<string> OwnFunctions(string handwritten) =>
    Regex.Matches(File.ReadAllText(handwritten), @"^HELLFORGE_API\s+[\w\s\*]+?\b(\w+)\s*\(", RegexOptions.Multiline)
        .Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);

static int Forward(string original, string module, string handwritten, string output)
{
    if (!Regex.IsMatch(module, @"^[A-Za-z0-9_]+$")) return Fail("The real module name must be a plain DLL name without extension.");
    var exports = PeTables.Exports(original);
    var own = OwnFunctions(handwritten);
    var unknown = own.Where(name => exports.All(e => e.Name != name)).ToList();
    if (unknown.Count > 0) return Fail("Hand-written functions the original does not export: " + string.Join(", ", unknown));
    var data = PeTables.DataExports(original);
    var definition = new StringBuilder().AppendLine($"LIBRARY {Path.GetFileName(original)}").AppendLine("EXPORTS");
    foreach (var export in exports.OrderBy(e => e.Ordinal))
        definition.AppendLine(own.Contains(export.Name) ? $"    {export.Name} @{export.Ordinal}"
            : $"    {export.Name} = {module}.{export.Name} @{export.Ordinal}{(data.Contains(export.Name) ? " DATA" : "")}");
    File.WriteAllText(output, definition.ToString());
    Console.WriteLine($"{Path.GetFileName(original)}: {exports.Count} exports, {own.Count} hand-written, {exports.Count - own.Count} forwarded to {module}.dll.");
    return 0;
}

// Same names and ordinals as the original; the hand-written ones are code, all others forward to module.sameName.
static int VerifyForward(string original, string module, string handwritten, string standIn)
{
    var own = OwnFunctions(handwritten);
    var expected = PeTables.Exports(original).ToDictionary(e => e.Name, e => e.Ordinal, StringComparer.Ordinal);
    var actual = PeTables.Exports(standIn).ToDictionary(e => e.Name, e => e.Ordinal, StringComparer.Ordinal);
    var forwarders = PeTables.Forwarders(standIn);
    var problems = new List<string>();
    foreach (var (name, ordinal) in expected)
    {
        if (!actual.TryGetValue(name, out var got) || got != ordinal) { problems.Add($"missing export {name} @{ordinal}"); continue; }
        forwarders.TryGetValue(name, out var target);
        if (own.Contains(name) ? target != null : target != $"{module}.{name}")
            problems.Add($"{name} {(own.Contains(name) ? "should be code but forwards to " + target : "should forward to " + module + "." + name)}");
    }
    problems.AddRange(actual.Keys.Where(name => !expected.ContainsKey(name)).Select(name => "extra export " + name));
    var modules = PeTables.ImportedModules(standIn);
    problems.AddRange(modules.Where(m => !IsSystemModule(m)).Select(m => "non-system import " + m));
    Console.WriteLine($"{Path.GetFileName(standIn)}: {actual.Count}/{expected.Count} exports, {forwarders.Count} forwarded; imports {string.Join(", ", modules)}");
    foreach (var problem in problems.Take(20)) Console.Error.WriteLine("  " + problem);
    return problems.Count == 0 ? 0 : 1;
}

static bool IsSystemModule(string name) =>
    name.StartsWith("api-ms-win-", StringComparison.OrdinalIgnoreCase) ||
    name.ToLowerInvariant() is "kernel32.dll" or "ntdll.dll" or "ucrtbase.dll" or "msvcrt.dll" or "advapi32.dll";

static int Fail(string message)
{
    Console.Error.WriteLine("HellForge.SdkShimGen: " + message.Trim());
    return 1;
}
