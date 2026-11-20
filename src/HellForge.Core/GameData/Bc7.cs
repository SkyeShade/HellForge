using System.Buffers.Binary;

namespace HellForge.Core.GameData;

// BC7 (DXGI 98, UNORM) in mode 6 only: one RGBA line per 4 x 4 block, 7-bit endpoints with a p-bit each, 4-bit
// weights. It is the format of the game's own weapon base_data (normal XY, AO, roughness), and keeps four correlated
// data channels far better than BC3's 565 colour block: on a weapon base_data the normal error roughly halves. The
// endpoints come from the block's principal axis, then a least-squares fit to the chosen weights, trying every p-bit
// pair; every channel weighs the same, since these textures hold data rather than colour.
public static class Bc7
{
    public const int Dxgi = 98;

    static readonly int[] Weights = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

    // The full mip chain (largest level first), each level box-filtered per channel as the other data formats are.
    public static byte[] Mips(int width, int height, byte[] rgba)
    {
        using var output = new MemoryStream();
        foreach (var (w, h, level) in IconImage.RgbaLevels(width, height, rgba, data: true)) output.Write(Level(w, h, level));
        return output.ToArray();
    }

    // One level, rows of blocks top to bottom; edge blocks repeat the last row and column.
    public static byte[] Level(int width, int height, byte[] rgba)
    {
        int bw = (Math.Max(width, 4) + 3) / 4, bh = (Math.Max(height, 4) + 3) / 4;
        var output = new byte[bw * bh * 16];
        Parallel.For(0, bh, by =>
        {
            Span<int> pixels = stackalloc int[64];
            for (var bx = 0; bx < bw; bx++)
            {
                for (var k = 0; k < 16; k++)
                {
                    var o = 4 * (Math.Min(4 * by + k / 4, height - 1) * width + Math.Min(4 * bx + k % 4, width - 1));
                    for (var c = 0; c < 4; c++) pixels[4 * k + c] = rgba[o + c];
                }
                EncodeBlock(pixels, output.AsSpan(16 * (by * bw + bx), 16));
            }
        });
        return output;
    }

    // One block of 16 RGBA pixels (row by row) into 16 bytes.
    public static void EncodeBlock(ReadOnlySpan<int> pixels, Span<byte> block)
    {
        Span<double> e0 = stackalloc double[4], e1 = stackalloc double[4];
        LineEndpoints(pixels, e0, e1);
        var best = new Choice();
        for (var round = 0; round < 3; round++)
        {
            TryPBits(pixels, e0, e1, best);
            if (best.Error == 0 || !Refit(pixels, best.Indices, e0, e1)) break;
        }
        Write(block, best.Q0, best.P0, best.Q1, best.P1, best.Indices);
    }

    sealed class Choice
    {
        public long Error = long.MaxValue;
        public int P0, P1;
        public int[] Q0 = new int[4], Q1 = new int[4], Indices = new int[16];
    }

    // Each p-bit pair on the same endpoints, keeping the best result over every call.
    static void TryPBits(ReadOnlySpan<int> pixels, ReadOnlySpan<double> e0, ReadOnlySpan<double> e1, Choice best)
    {
        Span<int> q0 = stackalloc int[4], q1 = stackalloc int[4], indices = stackalloc int[16];
        for (var p = 0; p < 4; p++)
        {
            int p0 = p & 1, p1 = p >> 1;
            Quantize(e0, p0, q0);
            Quantize(e1, p1, q1);
            var error = Assign(pixels, q0, p0, q1, p1, indices);
            if (error >= best.Error) continue;
            (best.Error, best.P0, best.P1) = (error, p0, p1);
            q0.CopyTo(best.Q0); q1.CopyTo(best.Q1); indices.CopyTo(best.Indices);
        }
    }

    // The ends of the pixels' principal axis through their mean: the line a single-subset block interpolates along.
    static void LineEndpoints(ReadOnlySpan<int> pixels, Span<double> e0, Span<double> e1)
    {
        Span<double> mean = stackalloc double[4], axis = stackalloc double[4];
        for (var k = 0; k < 16; k++)
            for (var c = 0; c < 4; c++) mean[c] += pixels[4 * k + c] / 16.0;
        PrincipalAxis(pixels, mean, axis);
        double lo = double.MaxValue, hi = double.MinValue;
        for (var k = 0; k < 16; k++)
        {
            double t = 0;
            for (var c = 0; c < 4; c++) t += (pixels[4 * k + c] - mean[c]) * axis[c];
            lo = Math.Min(lo, t); hi = Math.Max(hi, t);
        }
        for (var c = 0; c < 4; c++) { e0[c] = Math.Clamp(mean[c] + axis[c] * lo, 0, 255); e1[c] = Math.Clamp(mean[c] + axis[c] * hi, 0, 255); }
    }

    // Power iteration on the 4 x 4 covariance; a flat block keeps the start axis, which is harmless at zero spread.
    static void PrincipalAxis(ReadOnlySpan<int> pixels, ReadOnlySpan<double> mean, Span<double> axis)
    {
        Span<double> cov = stackalloc double[16], next = stackalloc double[4];
        for (var k = 0; k < 16; k++)
            for (var i = 0; i < 16; i++) cov[i] += (pixels[4 * k + i / 4] - mean[i / 4]) * (pixels[4 * k + i % 4] - mean[i % 4]);
        axis.Fill(0.5);
        for (var it = 0; it < 8; it++)
        {
            next.Clear();
            for (var i = 0; i < 16; i++) next[i / 4] += cov[i] * axis[i % 4];
            double n = 0;
            foreach (var v in next) n += v * v;
            if (n < 1e-12) return;
            n = Math.Sqrt(n);
            for (var i = 0; i < 4; i++) axis[i] = next[i] / n;
        }
    }

    static void Quantize(ReadOnlySpan<double> e, int p, Span<int> q)
    {
        for (var c = 0; c < 4; c++) q[c] = Math.Clamp((int)Math.Round((e[c] - p) / 2), 0, 127);
    }

    static int Value(int q, int p) => q << 1 | p;

    // Each pixel's nearest of the 16 interpolated colours; the total squared error.
    static long Assign(ReadOnlySpan<int> pixels, ReadOnlySpan<int> q0, int p0, ReadOnlySpan<int> q1, int p1, Span<int> indices)
    {
        Span<int> palette = stackalloc int[64];
        for (var i = 0; i < 16; i++)
            for (var c = 0; c < 4; c++)
                palette[4 * i + c] = ((64 - Weights[i]) * Value(q0[c], p0) + Weights[i] * Value(q1[c], p1) + 32) >> 6;
        long total = 0;
        for (var k = 0; k < 16; k++)
        {
            int nearest = 0, nearestError = int.MaxValue;
            for (var i = 0; i < 16; i++)
            {
                var d = 0;
                for (var c = 0; c < 4; c++) { var x = pixels[4 * k + c] - palette[4 * i + c]; d += x * x; }
                if (d < nearestError) (nearest, nearestError) = (i, d);
            }
            indices[k] = nearest;
            total += nearestError;
        }
        return total;
    }

    // Least-squares endpoints for the chosen weights; false when the weights are all the same (nothing to fit).
    static bool Refit(ReadOnlySpan<int> pixels, ReadOnlySpan<int> indices, Span<double> e0, Span<double> e1)
    {
        double aa = 0, ab = 0, bb = 0;
        Span<double> ax = stackalloc double[4], bx = stackalloc double[4];
        for (var k = 0; k < 16; k++)
        {
            double t = Weights[indices[k]] / 64.0, s = 1 - t;
            aa += s * s; ab += s * t; bb += t * t;
            for (var c = 0; c < 4; c++) { ax[c] += s * pixels[4 * k + c]; bx[c] += t * pixels[4 * k + c]; }
        }
        var det = aa * bb - ab * ab;
        if (Math.Abs(det) < 1e-9) return false;
        for (var c = 0; c < 4; c++)
        {
            e0[c] = Math.Clamp((bb * ax[c] - ab * bx[c]) / det, 0, 255);
            e1[c] = Math.Clamp((aa * bx[c] - ab * ax[c]) / det, 0, 255);
        }
        return true;
    }

    // Mode 6 layout, least significant bit first: mode bits 0000001, R0 R1 G0 G1 B0 B1 A0 A1 (7 bits each), P0, P1,
    // then 16 indices of 4 bits, the first (the anchor) stored in 3 with its top bit 0: endpoints swap when it is set.
    static void Write(Span<byte> block, Span<int> q0, int p0, Span<int> q1, int p1, Span<int> indices)
    {
        if (indices[0] >= 8)
        {
            for (var c = 0; c < 4; c++) (q0[c], q1[c]) = (q1[c], q0[c]);
            (p0, p1) = (p1, p0);
            for (var k = 0; k < 16; k++) indices[k] = 15 - indices[k];
        }
        UInt128 bits = 1u << 6;
        var at = 7;
        void Put(int value, int count) { bits |= (UInt128)(uint)value << at; at += count; }
        for (var c = 0; c < 4; c++) { Put(q0[c], 7); Put(q1[c], 7); }
        Put(p0, 1);
        Put(p1, 1);
        Put(indices[0], 3);
        for (var k = 1; k < 16; k++) Put(indices[k], 4);
        BinaryPrimitives.WriteUInt128LittleEndian(block, bits);
    }

    // Decodes a mode 6 block (for tests and tools): 16 RGBA pixels, row by row. Other modes throw.
    public static byte[] DecodeMode6(ReadOnlySpan<byte> block)
    {
        var bits = BinaryPrimitives.ReadUInt128LittleEndian(block);
        if ((int)(bits & 0x7F) != 0x40) throw new InvalidDataException("not a BC7 mode 6 block.");
        var at = 7;
        int Get(int count) { var v = (int)((bits >> at) & (((UInt128)1 << count) - 1)); at += count; return v; }
        int[] q0 = new int[4], q1 = new int[4];
        for (var c = 0; c < 4; c++) { q0[c] = Get(7); q1[c] = Get(7); }
        int p0 = Get(1), p1 = Get(1);
        var output = new byte[64];
        for (var k = 0; k < 16; k++)
        {
            var i = Get(k == 0 ? 3 : 4);
            for (var c = 0; c < 4; c++)
                output[4 * k + c] = (byte)(((64 - Weights[i]) * Value(q0[c], p0) + Weights[i] * Value(q1[c], p1) + 32) >> 6);
        }
        return output;
    }
}
