using System.Text;

namespace HellForge.Core.GameData;

// Whether a mod's compiled shader fits the game stage it goes into, read from their DXBC containers. The stage keeps the game's
// signatures and reflection and only its code changes, so the code has to use what the stage declares: the same inputs in the
// same registers, the same outputs, and only the stage's bindings at the stage's registers. The game's stage is the reference,
// so there are no lists to keep per host. This is for the mod's author, who gets the reason here and not a black screen or a
// device error in game. It is no safety check: a file's reflection need not match its code, and a spec with a shader counts as
// unchecked (DataSpecs.Unchecked).
public static class ShaderCheck
{
    sealed record Element(string Name, int Index, int Register, int Type, int Mask)
    {
        public string Semantic => $"{Name}{Index}";
    }

    sealed record Binding(string Name, int Kind, int Return, int Dimension, int Point, int Count, int Flags)
    {
        public string Slot => $"{Kind switch { 0 => 'b', 3 => 's', 1 or 2 or 5 or 7 => 't', _ => 'u' }}{Point}";
    }

    public static void Check(List<(string Name, byte[] Data)> game, List<(string Name, byte[] Data)> ours)
    {
        Version(U32(Part(game, "SHEX", "game"), 0), Part(ours, "SHEX", "ours"));
        if (ours.Any(c => c.Name == "SFI0" && c.Data.Any(b => b != 0)))
            throw new InvalidDataException("it uses optional GPU features (an SFI0 chunk, such as doubles), which the host stage does not declare.");
        Inputs(Signature(Part(game, "ISGN", "game")), Signature(Part(ours, "ISGN", "ours")));
        Outputs(Signature(Part(game, "OSGN", "game")), Signature(Part(ours, "OSGN", "ours")));
        if (ours.All(c => c.Name != "RDEF")) throw new InvalidDataException("it has no reflection (RDEF): compile it without stripping reflection, so its bindings can be checked.");
        Bindings(Part(game, "RDEF", "game"), Part(ours, "RDEF", "ours"));
    }

    static byte[] Part(List<(string Name, byte[] Data)> chunks, string name, string whose) =>
        chunks.FirstOrDefault(c => c.Name == name).Data ?? throw new InvalidDataException($"{(whose == "game" ? "the game's stage" : "the compiled shader")} has no {name} chunk.");

    // The version token: the program type (0 a pixel shader) in the top half, the shader model below.
    static void Version(int game, byte[] code)
    {
        var ours = U32(code, 0);
        if (ours >> 16 != 0) throw new InvalidDataException("it is not a pixel shader: compile it as ps_5_0.");
        if ((ours & 0xFF) != (game & 0xFF)) throw new InvalidDataException($"it is compiled for ps_{ours >> 4 & 15}_{ours & 15}, and the stage is ps_{game >> 4 & 15}_{game & 15}.");
    }

    // Every input it reads must be the stage's, in the stage's register, and within what that register holds. It may stop short
    // of the last ones, since the code reads registers of the kept signature.
    static void Inputs(List<Element> game, List<Element> ours)
    {
        var names = string.Join(", ", game.Select(e => $"v{e.Register} {e.Semantic}"));
        foreach (var input in ours)
        {
            var theirs = game.FirstOrDefault(e => Same(e, input)) ?? throw new InvalidDataException($"its input {input.Semantic} is not one of the stage's: {names}.");
            if (theirs.Register != input.Register)
                throw new InvalidDataException($"its input {input.Semantic} is in v{input.Register}, and the stage has it in v{theirs.Register}: declare the stage's inputs in its order ({names}).");
            if (theirs.Type != input.Type || (input.Mask & ~theirs.Mask) != 0)
                throw new InvalidDataException($"its input {input.Semantic} is not the type or size the stage has (the stage has {Components(theirs.Mask)}, it asks for {Components(input.Mask)}).");
        }
    }

    // Every output must be written, as the stage's: the kept signature promises them to the pass, which blends each one.
    static void Outputs(List<Element> game, List<Element> ours)
    {
        var names = string.Join(", ", game.Select(e => $"{e.Semantic} {Components(e.Mask)}"));
        foreach (var output in game)
            if (!ours.Any(e => Same(e, output))) throw new InvalidDataException($"it does not write {output.Semantic}: the stage's outputs are {names}, and all of them must be written.");
        foreach (var output in ours)
        {
            var theirs = game.FirstOrDefault(e => Same(e, output)) ?? throw new InvalidDataException($"it writes {output.Semantic}, and the stage's outputs are {names}.");
            if (theirs.Register != output.Register || theirs.Type != output.Type || theirs.Mask != output.Mask)
                throw new InvalidDataException($"its output {output.Semantic} is {Components(output.Mask)} in o{output.Register}, and the stage's outputs are {names}.");
        }
    }

    static bool Same(Element a, Element b) => a.Index == b.Index && a.Name.Equals(b.Name, StringComparison.OrdinalIgnoreCase);

    static string Components(int mask) => string.Concat("xyzw".Where((_, bit) => (mask >> bit & 1) != 0));

    static void Bindings(byte[] game, byte[] ours)
    {
        var (gameBindings, gameSizes) = Reflection(game);
        var (ourBindings, ourSizes) = Reflection(ours);
        var names = string.Join(", ", gameBindings.Select(b => $"{b.Slot} {b.Name}"));
        foreach (var binding in ourBindings)
        {
            if (binding.Name == "$Globals") throw new InvalidDataException("it has global variables (a $Globals cbuffer), and the stage has no such constants: make them static const.");
            var theirs = gameBindings.FirstOrDefault(b => b.Name == binding.Name)
                ?? throw new InvalidDataException($"it binds {binding.Name} at {binding.Slot}, which the stage does not have: a mod's shader has only the stage's bindings ({names}).");
            if (theirs.Slot != binding.Slot) throw new InvalidDataException($"it binds {binding.Name} at {binding.Slot}, and the stage has it at {theirs.Slot}.");
            if (theirs.Kind != binding.Kind || theirs.Return != binding.Return || theirs.Dimension != binding.Dimension || theirs.Count != binding.Count ||
                (theirs.Flags & 2) != (binding.Flags & 2))
                throw new InvalidDataException($"it declares {binding.Name} at {binding.Slot} as another kind of resource than the stage's.");
            if (binding.Kind == 0 && ourSizes.GetValueOrDefault(binding.Name) > gameSizes.GetValueOrDefault(binding.Name))
                throw new InvalidDataException($"its cbuffer {binding.Name} is {ourSizes[binding.Name]} bytes, and the stage's is {gameSizes[binding.Name]}.");
        }
    }

    // ISGN and OSGN: a count, then from +8 24 bytes per element: name, semantic index, system value, component type, register, masks.
    static List<Element> Signature(byte[] chunk)
    {
        var elements = new List<Element>();
        for (var i = 0; i < U32(chunk, 0); i++)
        {
            var at = 8 + 24L * i;
            elements.Add(new(Text(chunk, U32(chunk, at)), U32(chunk, at + 4), U32(chunk, at + 16), U32(chunk, at + 12), U32(chunk, at + 20) & 0xFF));
        }
        return elements;
    }

    // RDEF: cbuffer count and offset (24 bytes each: name, variables, their offset, size), binding count and offset (32 bytes each:
    // name, type, return type, dimension, samples, point, count, flags). Shader model 5.0's layout, whose version Check compares first.
    static (List<Binding> Bindings, Dictionary<string, int> Sizes) Reflection(byte[] chunk)
    {
        var sizes = new Dictionary<string, int>(StringComparer.Ordinal);
        var (buffers, buffersAt, count, bindingsAt) = (U32(chunk, 0), U32(chunk, 4), U32(chunk, 8), U32(chunk, 12));
        for (var i = 0; i < buffers; i++) sizes[Text(chunk, U32(chunk, buffersAt + 24L * i))] = U32(chunk, buffersAt + 24L * i + 12);
        var bindings = new List<Binding>();
        for (var i = 0; i < count; i++)
        {
            var at = bindingsAt + 32L * i;
            bindings.Add(new(Text(chunk, U32(chunk, at)), U32(chunk, at + 4), U32(chunk, at + 8), U32(chunk, at + 12), U32(chunk, at + 20), U32(chunk, at + 24), U32(chunk, at + 28)));
        }
        return (bindings, sizes);
    }

    static int U32(byte[] chunk, long at) =>
        at >= 0 && at + 4 <= chunk.Length && BitConverter.ToInt32(chunk, (int)at) is >= 0 and var value ? value : throw new InvalidDataException("a chunk of the shader is broken.");

    static string Text(byte[] chunk, int at)
    {
        var end = at < chunk.Length ? Array.IndexOf(chunk, (byte)0, at) : -1;
        return end >= 0 ? Encoding.ASCII.GetString(chunk, at, end - at) : throw new InvalidDataException("a chunk of the shader is broken.");
    }
}
