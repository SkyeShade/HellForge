using System.Globalization;
using System.Text;
using System.Text.Json;

namespace HellForge.Core.GameData;

// A stage of a shader host: its place in the root's GPU part, and the digest the game's container has there.
public sealed record HostStage(int Offset, int Size, byte[] Digest)
{
    // Whether the GPU part has this stage: offsets are per game build, so anything else is a build the catalogue does not know.
    public void Check(byte[] gpu)
    {
        if ((long)Offset + 28 > gpu.Length || BitConverter.ToInt32(gpu, Offset + 24) != Size || !gpu.AsSpan(Offset + 4, 16).SequenceEqual(Digest))
            throw new InvalidDataException($"the game's shader stage at 0x{Offset:X} is not the one HellForge's shader hosts list for this game build.");
    }
}

// Child: a child material of the root and where it names its parent. Mesh: whether the host draws a mesh the mod brings, or the
// unit's own box.
public sealed record ShaderHost(string Name, ulong Root, IReadOnlyList<HostStage> Stages, (ulong Material, int ParentAt)? Child, ulong Unit, ulong UnitMaterial, int UnitMaterialAt,
    bool Mesh, IReadOnlyList<string> Bindings);

// The game's materials a mod's pixel shader can go into, for the supported game build (mappings/shader-hosts.json,
// scripts/ExportShaderHosts.py). A data spec's "shader" resource names a host and HellForge builds the copies it needs, so the
// mod's spec holds no offsets: the host's root material with the mod's code in every stage, its child when the root cannot be
// drawn itself, and a unit that uses them. docs/modding/shaders.md describes the hosts.
public sealed class ShaderHosts(IReadOnlyDictionary<string, ShaderHost> hosts)
{
    static readonly Lazy<ShaderHosts> Embedded = new(() =>
    {
        using var stream = typeof(ShaderHosts).Assembly.GetManifestResourceStream("HellForge.Core.mappings.shader-hosts.json")!;
        using var reader = new StreamReader(stream);
        return Parse(reader.ReadToEnd(), SupportedBuild.Current.BuildId);
    });

    public static ShaderHosts Current => Embedded.Value;
    public IReadOnlyDictionary<string, ShaderHost> Hosts { get; } = hosts;

    public static ShaderHosts Parse(string json, string build)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        var listed = root.GetProperty("build").GetString();
        if (listed != build) throw new InvalidDataException($"the shader hosts are for game build {listed}, and this is build {build}: HellForge needs an update.");
        var hosts = new Dictionary<string, ShaderHost>(StringComparer.Ordinal);
        foreach (var host in root.GetProperty("hosts").EnumerateObject())
        {
            var value = host.Value;
            var unit = value.GetProperty("unit");
            var stages = value.GetProperty("stages").EnumerateArray().Select(s => new HostStage((int)Hex(s.GetProperty("offset")), s.GetProperty("size").GetInt32(),
                Convert.FromHexString(s.GetProperty("digest").GetString()!))).ToList();
            (ulong, int)? child = value.TryGetProperty("child", out var c) ? (Hex(c.GetProperty("copy")), (int)Hex(c.GetProperty("parentAt"))) : null;
            hosts[host.Name] = new(host.Name, Hex(value.GetProperty("root")), stages, child, Hex(unit.GetProperty("copy")), Hex(unit.GetProperty("material")), (int)Hex(unit.GetProperty("at")),
                value.GetProperty("mesh").GetBoolean(), [.. value.GetProperty("bindings").EnumerateArray().Select(b => b.GetString()!)]);
        }
        return new(hosts);
    }

    static ulong Hex(JsonElement e) => ulong.Parse(e.GetString()!.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture);

    // The spec with each "shader" resource as the copies it stands for, which the builder then treats like any others.
    public DataSpec Expand(DataSpec spec) => spec with
    {
        Patches = [.. spec.Patches.Select(p => p with { Resources = [.. p.Resources.SelectMany(r => r is ShaderResource shader ? Copies(shader) : [r])] })]
    };

    // <name> is the unit and <name>/material the material on it; with a child, that is the child and <name>/root the root.
    public IReadOnlyList<DataResource> Copies(ShaderResource shader)
    {
        var host = Hosts[shader.Host];
        var material = shader.Name + "/material";
        var coded = host.Child == null ? material : shader.Name + "/root";
        var copies = new List<DataResource> { Copy(shader, ResourceTypes.Material, host.Root, coded, [], null, [.. host.Stages.Select(s => new ShaderFile(s.Offset, shader.Dxbc, s))]) };
        if (host.Child is { } child) copies.Add(Copy(shader, ResourceTypes.Material, child.Material, material, [new(host.Root, Hash(coded), [child.ParentAt])], null, null));
        copies.Add(Copy(shader, ResourceTypes.Unit, host.Unit, shader.Name, [new(host.UnitMaterial, Hash(material), [host.UnitMaterialAt])], shader.Mesh, null));
        return copies;
    }

    static CopyResource Copy(ShaderResource shader, ulong type, ulong from, string name, IReadOnlyList<Replacement> replace, MeshFile? mesh, IReadOnlyList<ShaderFile>? shaders) =>
        new(shader.Where, type, from, name, new Dictionary<int, ulong>(), replace, null, null, null, mesh, Shaders: shaders);

    static ulong Hash(string name) => EngineHash.Murmur64A(Encoding.UTF8.GetBytes(name));
}
