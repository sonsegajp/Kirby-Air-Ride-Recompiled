// A scene of Ship of Harkinian's (Ocarina of Time on PC: its oot.o2r, which SoH makes from the player's own ROM) as a map:
// the scene's rooms drawn by running their display lists (F3DEX2) through a small interpreter into triangles with their
// textures and vertex colours, its collision as a mesh of its own, its start positions as the riders' spawns. Nothing of
// the game's is kept here: everything is read from the player's archive when the map is made.
//
// The archive (a zip): every file a resource - a 64-byte header (byte order, type, version), then its data. Display lists
// name what they use (vertices, textures, other lists) by a hash of its path (CRC-64, as libultraship's).
using IONET;
using IONET.Core;
using IONET.Core.Model;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Processing;
using System.IO.Compression;
using System.Numerics;
using System.Text;
using System.Text.Json;

public class O2rArchive : IDisposable
{
    readonly ZipArchive zip;
    readonly Dictionary<ulong, string> byHash = new();
    readonly Dictionary<string, ZipArchiveEntry> entries = new();

    public O2rArchive(string path)
    {
        zip = ZipFile.OpenRead(path);
        foreach (var e in zip.Entries)
        {
            entries[e.FullName] = e;
            byHash[Hash(e.FullName)] = e.FullName;
        }
    }

    public void Dispose() => zip.Dispose();

    public IEnumerable<string> Names => entries.Keys;
    public bool Has(string path) => entries.ContainsKey(path);
    public string PathOf(ulong hash) => byHash.TryGetValue(hash, out var p) ? p : null;

    // A resource's data, past its header.
    public byte[] Read(string path)
    {
        if (!entries.TryGetValue(path, out var e)) return null;
        using var s = e.Open();
        using var ms = new MemoryStream();
        s.CopyTo(ms);
        var all = ms.ToArray();
        return all.Length < 64 ? Array.Empty<byte>() : all[64..];
    }

    // libultraship's CRC64 of a path (ECMA-182, MSB first, no final inversion).
    static readonly ulong[] table = MakeTable();
    static ulong[] MakeTable()
    {
        var t = new ulong[256];
        for (ulong i = 0; i < 256; i++)
        {
            ulong c = i << 56;
            for (int k = 0; k < 8; k++) c = (c & 0x8000000000000000UL) != 0 ? (c << 1) ^ 0x42F0E1EBA9EA3693UL : c << 1;
            t[i] = c;
        }
        return t;
    }
    public static ulong Hash(string s)
    {
        ulong crc = ulong.MaxValue;
        foreach (byte b in Encoding.ASCII.GetBytes(s)) crc = table[(byte)(crc >> 56) ^ b] ^ (crc << 8);
        return crc;
    }
}

// Reads a resource's fields (little-endian, as SoH writes them on PC).
class ResReader
{
    readonly byte[] d;
    public int Pos;
    public ResReader(byte[] data) { d = data; }
    public bool End => Pos >= d.Length;
    public byte U8() => d[Pos++];
    public sbyte S8() => (sbyte)d[Pos++];
    public short S16() { var v = BitConverter.ToInt16(d, Pos); Pos += 2; return v; }
    public ushort U16() { var v = BitConverter.ToUInt16(d, Pos); Pos += 2; return v; }
    public int S32() { var v = BitConverter.ToInt32(d, Pos); Pos += 4; return v; }
    public uint U32() { var v = BitConverter.ToUInt32(d, Pos); Pos += 4; return v; }
    public string Str() { int n = S32(); var s = Encoding.ASCII.GetString(d, Pos, n); Pos += n; return s; }
}

public static partial class SohScene
{
    // One triangle as drawn: its corners (position, texture coordinate in texels of its texture, colour) and its material.
    record struct Corner(Vector3 P, Vector2 UV, Vector4 C);
    // A material: a texture as its tile draws it (wrap modes), or two blended (OoT's terrain: two textures mixed by a
    // constant) baked into one - the second's coordinates the first's times A plus O, over a period of P texels of the first.
    record Material(string Tex, string Tlut, int Cms, int Cmt, bool TexAlpha,
                    string Tex1 = null, string Tlut1 = null, int Cms1 = 0, int Cmt1 = 0, float Mix = 0,
                    float As = 1, float At = 1, float Os = 0, float Ot = 0, int Pw = 0, int Ph = 0);

    // ---- textures (N64 formats; SoH keeps them as the ROM has them, big-endian) ----
    // texture type (libultraship's TextureType): 1 RGBA32, 2 RGBA16, 3 CI4, 4 CI8, 5 I4, 6 I8, 7 IA4, 8 IA8, 9 IA16
    static Rgba32 FromRgba16(int v) => new((byte)((v >> 11 & 31) * 255 / 31), (byte)((v >> 6 & 31) * 255 / 31), (byte)((v >> 1 & 31) * 255 / 31), (byte)((v & 1) != 0 ? 255 : 0));

    static (int type, int w, int h, byte[] px) ReadTexture(O2rArchive a, string path)
    {
        var d = a.Read(path);
        if (d == null || d.Length < 16) return (0, 0, 0, null);
        var r = new ResReader(d);
        int type = (int)r.U32(), w = (int)r.U32(), h = (int)r.U32();
        int size = (int)r.U32();
        // (version 1 has flags and two scales before the size: told apart by what is left)
        if (16 + size != d.Length && d.Length >= 28) { r.Pos = 24; size = (int)r.U32(); }
        return (type, w, h, d[r.Pos..Math.Min(d.Length, r.Pos + size)]);
    }

    static Image<Rgba32> Decode(O2rArchive a, string tex, string tlut, bool texAlpha)
    {
        var (type, w, h, px) = ReadTexture(a, tex);
        if (px == null || w == 0 || h == 0) return null;
        Rgba32[] pal = null;
        if (type == 3 || type == 4)
        {
            pal = new Rgba32[256];
            if (tlut != null)
            {
                var (_, pw, ph, pp) = ReadTexture(a, tlut);
                for (int i = 0; i < Math.Min(256, pp.Length / 2); i++) pal[i] = FromRgba16(pp[i * 2] << 8 | pp[i * 2 + 1]);
            }
            else for (int i = 0; i < 256; i++) pal[i] = new Rgba32((byte)i, (byte)i, (byte)i, 255);
        }
        var img = new Image<Rgba32>(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                Rgba32 c;
                int Nib(int k) => k / 2 < px.Length ? (k % 2 == 0 ? px[k / 2] >> 4 : px[k / 2] & 15) : 0;
                int B(int k) => k < px.Length ? px[k] : 0;
                switch (type)
                {
                    case 1: c = new Rgba32((byte)B(i * 4), (byte)B(i * 4 + 1), (byte)B(i * 4 + 2), (byte)B(i * 4 + 3)); break;
                    case 2: c = FromRgba16(B(i * 2) << 8 | B(i * 2 + 1)); break;
                    case 3: c = pal[Nib(i)]; break;
                    case 4: c = pal[B(i)]; break;
                    case 5: { byte v = (byte)(Nib(i) * 17); c = new Rgba32(v, v, v, v); break; }
                    case 6: { byte v = (byte)B(i); c = new Rgba32(v, v, v, v); break; }
                    case 7: { int n = Nib(i); byte v = (byte)((n >> 1) * 255 / 7); c = new Rgba32(v, v, v, (byte)((n & 1) != 0 ? 255 : 0)); break; }
                    case 8: { int n = B(i); byte v = (byte)((n >> 4) * 17); c = new Rgba32(v, v, v, (byte)((n & 15) * 17)); break; }
                    case 9: { byte v = (byte)B(i * 2); c = new Rgba32(v, v, v, (byte)B(i * 2 + 1)); break; }
                    default: c = new Rgba32(255, 0, 255, 255); break;
                }
                if (!texAlpha) c.A = 255;
                img[x, y] = c;
            }
        return img;
    }

    // Two textures blended into one, over the period they repeat together (in texels of the first; finer, if the second is).
    static Image<Rgba32> Bake(O2rArchive a, Material m)
    {
        using var i0 = Decode(a, m.Tex, m.Tlut, m.TexAlpha);
        using var i1 = Decode(a, m.Tex1, m.Tlut1, m.TexAlpha);
        if (i0 == null) return null;
        if (i1 == null) return i0.Clone();
        float qs = Math.Clamp(m.As, 1, 4), qt = Math.Clamp(m.At, 1, 4);
        while (m.Pw * qs > 512 && qs > 1) qs /= 2;
        while (m.Ph * qt > 512 && qt > 1) qt /= 2;
        int bw = Math.Max(1, (int)(m.Pw * qs)), bh = Math.Max(1, (int)(m.Ph * qt));
        static int Wrap(float s, int w, int cm)
        {
            if ((cm & 2) != 0) return Math.Clamp((int)MathF.Floor(s), 0, w - 1);
            int f = (int)MathF.Floor(s);
            int r = ((f % w) + w) % w;
            if ((cm & 1) != 0 && ((int)MathF.Floor((float)f / w) & 1) != 0) r = w - 1 - r;
            return r;
        }
        var img = new Image<Rgba32>(bw, bh);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
            {
                float s0 = (x + 0.5f) / qs, t0 = (y + 0.5f) / qt;
                var c0 = i0[Wrap(s0, i0.Width, m.Cms), Wrap(t0, i0.Height, m.Cmt)].ToScaledVector4();
                var c1 = i1[Wrap(s0 * m.As + m.Os, i1.Width, m.Cms1), Wrap(t0 * m.At + m.Ot, i1.Height, m.Cmt1)].ToScaledVector4();
                img[x, y] = new Rgba32(Vector4.Lerp(c0, c1, m.Mix));
            }
        return img;
    }

    // ---- the display list interpreter (F3DEX2, with libultraship's commands that name resources by hash) ----
    class Rsp
    {
        public O2rArchive A;
        public Action<string> Log;
        public List<(Corner a, Corner b, Corner c, Material m)> Tris = new();
        public List<string> TriPart = new();  // (each triangle's scene: a mesh per scene, culled as a part of the map)
        public string Part = "";
        public Dictionary<Material, string> Materials = new();  // (each its name)
        public Dictionary<int, int> Ops = new();
        public Dictionary<Material, string> Combiners = new();
        public Matrix4x4 Xf = Matrix4x4.Identity;  // where what is drawn is placed (an actor's position and turn)
        public Dictionary<int, string> SegTextures;  // textures the scene's draw code puts in segments (by day)
        // The scene's lights by day (its first light settings): what lit surfaces - their vertices normals, not colours - are
        // shaded by: an ambient colour and two directional lights (each the way to the light).
        public (Vector3 amb, Vector3 d1, Vector3 c1, Vector3 d2, Vector3 c2) Light = (new(0.6f), Vector3.Normalize(new(0.4f, 0.8f, 0.45f)), new(0.5f), -Vector3.Normalize(new(0.4f, 0.8f, 0.45f)), new(0.2f));
        readonly (Vector3 p, short s, short t, byte r, byte g, byte b, byte al)[] vbuf = new (Vector3, short, short, byte, byte, byte, byte)[64];
        uint geom = 0x00000400;  // (back faces culled, as the game sets scenes up to draw)
        uint otherH = 1u << 20, otherL;  // (two-cycle, as the game sets scenes up to draw)
        ulong combine;
        Vector4 prim = Vector4.One, env = new(128 / 255f);  // (env 128: as Hyrule Field's scene sets it)
        float primLod = 0.5f;
        float texS = 1, texT = 1;
        int texTile;
        bool texOn;
        string timg;
        readonly Dictionary<int, string> tmem = new();      // what is loaded where in texture memory
        readonly Dictionary<int, string> tlutAt = new();    // palettes, by their texture memory address
        readonly int[] tileTmem = new int[8], tileFmt = new int[8], tilePal = new int[8], tileCms = new int[8], tileCmt = new int[8], tileShiftS = new int[8], tileShiftT = new int[8];
        readonly float[] tileUls = new float[8], tileUlt = new float[8];
        public int Missing;

        // What the game's actors set before their lists (Gfx_SetupDL_25Opa/Xlu, z_rcp.c): texturing on, the combiner
        // G_CC_MODULATEIDECALA then G_CC_MODULATEIA_PRIM2, two-cycle, lit, back faces culled - and the colours the
        // actor's draw sets (white where it sets none).
        public void Setup25(Vector4? primColor = null, Vector4? envColor = null)
        {
            texOn = true; texS = texT = 1; texTile = 0;
            combine = 0x127E03UL << 32 | 0xFF0FF3FFUL;
            otherH = 1u << 20; otherL = 0;
            geom = 0x00000001 | 0x00000004 | 0x00000400 | 0x00010000 | 0x00020000 | 0x00200000;
            prim = primColor ?? Vector4.One; primLod = 0.5f;
            env = envColor ?? Vector4.One;
        }

        // The drawing state (what the actors' setup changes), kept to give back to the scenes' lists after them.
        public (uint, uint, uint, ulong, Vector4, Vector4, float, float, float, int, bool) State
        {
            get => (geom, otherH, otherL, combine, prim, env, primLod, texS, texT, texTile, texOn);
            set => (geom, otherH, otherL, combine, prim, env, primLod, texS, texT, texTile, texOn) = value;
        }

        // Vertices an actor's draw loads itself (gSPVertex from its code, a list then drawing them).
        public void LoadVertices(string path, int n, int v0)
        {
            var vd = A.Read(path);
            if (vd == null) { Missing++; return; }
            int at = 8;
            for (int i = 0; i < n && at + 16 <= vd.Length && v0 + i < vbuf.Length; i++, at += 16)
                vbuf[v0 + i] = (Vector3.Transform(new Vector3(BitConverter.ToInt16(vd, at), BitConverter.ToInt16(vd, at + 2), BitConverter.ToInt16(vd, at + 4)), Xf),
                                BitConverter.ToInt16(vd, at + 8), BitConverter.ToInt16(vd, at + 10), vd[at + 12], vd[at + 13], vd[at + 14], vd[at + 15]);
        }

        public void Run(string path, int depth = 0)
        {
            if (depth > 32) return;
            var d = A.Read(path);
            if (d == null) { Missing++; return; }
            // ucode byte, then the commands from the first 8-byte boundary (of the file, header included)
            int pos = 8;
            while (pos + 8 <= d.Length)
            {
                uint w0 = BitConverter.ToUInt32(d, pos), w1 = BitConverter.ToUInt32(d, pos + 4);
                pos += 8;
                int op = (int)(w0 >> 24);
                Ops[op] = Ops.GetValueOrDefault(op) + 1;
                ulong hash = 0;
                if (op is 0x20 or 0x31 or 0x32 or 0x33 or 0x35 or 0x36 or 0x42)
                {
                    if (pos + 8 > d.Length) break;
                    hash = (ulong)BitConverter.ToUInt32(d, pos) << 32 | BitConverter.ToUInt32(d, pos + 4);
                    pos += 8;
                }
                switch (op)
                {
                    case 0xDF: return;                                   // G_ENDDL
                    case 0x32:                                           // G_VTX by hash (w1: byte offset)
                    {
                        int n = (int)(w0 >> 12 & 0xFF), v0 = (int)(w0 >> 1 & 0x7F) - n;
                        var vp = A.PathOf(hash);
                        if (vp == null) { Missing++; break; }
                        var vd = A.Read(vp);
                        // an array resource: its element type, its count, then the vertices (16 bytes each)
                        int at = 8 + (int)w1;
                        for (int i = 0; i < n && at + 16 <= vd.Length && v0 + i < vbuf.Length && v0 + i >= 0; i++, at += 16)
                        {
                            var p = Vector3.Transform(new Vector3(BitConverter.ToInt16(vd, at), BitConverter.ToInt16(vd, at + 2), BitConverter.ToInt16(vd, at + 4)), Xf);
                            vbuf[v0 + i] = (p, BitConverter.ToInt16(vd, at + 8), BitConverter.ToInt16(vd, at + 10), vd[at + 12], vd[at + 13], vd[at + 14], vd[at + 15]);
                        }
                        break;
                    }
                    case 0x05: Tri((int)(w0 >> 16 & 0xFF) / 2, (int)(w0 >> 8 & 0xFF) / 2, (int)(w0 & 0xFF) / 2); break;
                    case 0x06:
                    case 0x07:
                        Tri((int)(w0 >> 16 & 0xFF) / 2, (int)(w0 >> 8 & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
                        Tri((int)(w1 >> 16 & 0xFF) / 2, (int)(w1 >> 8 & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
                        break;
                    case 0x31:                                           // G_DL by hash: a call, or a branch (the rest of this list not run)
                    case 0x35:                                           // G_BRANCH_Z (the nearer, finer list taken)
                    {
                        var dp = A.PathOf(hash);
                        if (dp == null) { Missing++; break; }
                        Run(dp, depth + 1);
                        if (op == 0x35 || (w0 >> 16 & 0xFF) != 0) return;
                        break;
                    }
                    case 0xDE: break;                                    // G_DL to a segment (scrolling textures and the like): skipped
                    case 0xD9: geom = (geom & (w0 & 0xFFFFFF)) | w1; break;
                    case 0xE2: { int sft = 32 - (int)(w0 >> 8 & 0xFF) - 1 - (int)(w0 & 0xFF); uint mask = (uint)(((1UL << (int)((w0 & 0xFF) + 1)) - 1) << sft); otherL = (otherL & ~mask) | w1; break; }
                    case 0xE3: { int sft = 32 - (int)(w0 >> 8 & 0xFF) - 1 - (int)(w0 & 0xFF); uint mask = (uint)(((1UL << (int)((w0 & 0xFF) + 1)) - 1) << sft); otherH = (otherH & ~mask) | w1; break; }
                    case 0xFC: combine = (ulong)(w0 & 0xFFFFFF) << 32 | w1; break;
                    case 0xFA: prim = Rgba(w1); primLod = (w0 & 0xFF) / 255f; break;
                    case 0xFB: env = Rgba(w1); break;
                    case 0xD7:                                           // G_TEXTURE
                        texS = (w1 >> 16) / 65536f; texT = (w1 & 0xFFFF) / 65536f;
                        texTile = (int)(w0 >> 8 & 7); texOn = (w0 & 0xFE) != 0;
                        break;
                    case 0x20: timg = A.PathOf(hash); if (timg == null) Missing++; break;
                    case 0xFD:                                           // an image in a segment: the scene's (its draw code's), else unknown
                        timg = SegTextures != null && SegTextures.TryGetValue((int)(w1 >> 24), out var segTex) && A.Has(segTex) ? segTex : null;
                        break;
                    case 0xF5:                                           // G_SETTILE
                    {
                        int t = (int)(w1 >> 24 & 7);
                        tileFmt[t] = (int)(w0 >> 21 & 7); tileTmem[t] = (int)(w0 & 0x1FF);
                        tilePal[t] = (int)(w1 >> 20 & 15);
                        tileCmt[t] = (int)(w1 >> 18 & 3); tileShiftT[t] = (int)(w1 >> 10 & 15);
                        tileCms[t] = (int)(w1 >> 8 & 3); tileShiftS[t] = (int)(w1 & 15);
                        break;
                    }
                    case 0xF2:                                           // G_SETTILESIZE
                    {
                        int t = (int)(w1 >> 24 & 7);
                        tileUls[t] = (w0 >> 12 & 0xFFF) / 4f; tileUlt[t] = (w0 & 0xFFF) / 4f;
                        break;
                    }
                    case 0xF3:                                           // G_LOADBLOCK
                    case 0xF4:                                           // G_LOADTILE
                        tmem[tileTmem[w1 >> 24 & 7]] = timg;
                        break;
                    case 0xF0:                                           // G_LOADTLUT
                        tlutAt[tileTmem[w1 >> 24 & 7]] = timg;
                        break;
                }
            }
        }

        static Vector4 Rgba(uint v) => new((v >> 24 & 0xFF) / 255f, (v >> 16 & 0xFF) / 255f, (v >> 8 & 0xFF) / 255f, (v & 0xFF) / 255f);

        // The colour combiner, as what multiplies each texture and what is added: out = k0 * texel0 + k1 * texel1 + m.
        (Vector4 k0, Vector4 k1, Vector4 m) Combine(Vector4 shade)
        {
            bool twoCycle = (otherH >> 20 & 3) == 1;
            Vector4 Eval(Vector4 t0, Vector4 t1, Vector4 comb, int cyc)
            {
                uint w0 = (uint)(combine >> 32), w1 = (uint)combine;
                int a, b, c, d, aa, ab, ac, ad;
                if (cyc == 0) { a = (int)(w0 >> 20 & 15); c = (int)(w0 >> 15 & 31); aa = (int)(w0 >> 12 & 7); ac = (int)(w0 >> 9 & 7); b = (int)(w1 >> 28 & 15); ab = (int)(w1 >> 12 & 7); d = (int)(w1 >> 15 & 7); ad = (int)(w1 >> 9 & 7); }
                else { a = (int)(w0 >> 5 & 15); c = (int)(w0 & 31); aa = (int)(w1 >> 21 & 7); ac = (int)(w1 >> 18 & 7); b = (int)(w1 >> 24 & 15); ab = (int)(w1 >> 3 & 7); d = (int)(w1 >> 6 & 7); ad = (int)(w1 & 7); }
                Vector3 Col(int sel, int which)  // which: 0 a, 1 b, 2 c, 3 d
                {
                    switch (sel)
                    {
                        case 0: return new Vector3(comb.X, comb.Y, comb.Z);
                        case 1: return new Vector3(t0.X, t0.Y, t0.Z);
                        case 2: return new Vector3(t1.X, t1.Y, t1.Z);
                        case 3: return new Vector3(prim.X, prim.Y, prim.Z);
                        case 4: return new Vector3(shade.X, shade.Y, shade.Z);
                        case 5: return new Vector3(env.X, env.Y, env.Z);
                        case 6: return which == 0 || which == 2 ? Vector3.One : Vector3.Zero;
                        case 7: return which == 2 ? new Vector3(comb.W) : Vector3.Zero;
                        case 8: return which == 2 ? new Vector3(t0.W) : Vector3.Zero;
                        case 9: return which == 2 ? new Vector3(t1.W) : Vector3.Zero;
                        case 10: return which == 2 ? new Vector3(prim.W) : Vector3.Zero;
                        case 11: return which == 2 ? new Vector3(shade.W) : Vector3.Zero;
                        case 12: return which == 2 ? new Vector3(env.W) : Vector3.Zero;
                        case 14: return which == 2 ? new Vector3(primLod) : Vector3.Zero;
                        default: return Vector3.Zero;  // (13, LOD fraction: near, 0)
                    }
                }
                float Alp(int sel, bool isC)
                {
                    switch (sel)
                    {
                        case 0: return isC ? 0f : comb.W;
                        case 1: return t0.W;
                        case 2: return t1.W;
                        case 3: return prim.W;
                        case 4: return shade.W;
                        case 5: return env.W;
                        case 6: return isC ? primLod : 1f;
                        default: return 0f;
                    }
                }
                var rgb = (Col(a, 0) - Col(b, 1)) * Col(c, 2) + Col(d, 3);
                float al = (Alp(aa, false) - Alp(ab, false)) * Alp(ac, true) + Alp(ad, false);
                return new Vector4(rgb, al);
            }
            Vector4 Out(Vector4 t0, Vector4 t1)
            {
                var c0 = Eval(t0, t1, Vector4.Zero, 0);
                return twoCycle ? Eval(t0, t1, c0, 1) : c0;
            }
            var z = Out(Vector4.Zero, Vector4.Zero);
            return (Out(Vector4.One, Vector4.Zero) - z, Out(Vector4.Zero, Vector4.One) - z, z);
        }

        static float Lum(Vector4 v) => MathF.Abs(v.X) + MathF.Abs(v.Y) + MathF.Abs(v.Z);

        // A tile's texture and palette (colour-indexed: its palette where its tile says, 16 colours each).
        (string tex, string tlut) TileTexture(int t)
        {
            if (!tmem.TryGetValue(tileTmem[t], out var tex) || tex == null) return (null, null);
            string tlut = null;
            if (tileFmt[t] == 2 && !tlutAt.TryGetValue(0x100 + tilePal[t] * 16, out tlut)) tlutAt.TryGetValue(0x100, out tlut);
            return (tex, tlut);
        }

        // a lit vertex's colour: its normal (the colour bytes, signed) turned as it is placed, under the scene's lights
        Vector3 Lit(byte r, byte g, byte b)
        {
            var n = Vector3.TransformNormal(new Vector3((sbyte)r, (sbyte)g, (sbyte)b), Xf);
            if (n.LengthSquared() < 1e-6f) return Light.amb;
            n = Vector3.Normalize(n);
            var c = Light.amb + Light.c1 * MathF.Max(0, Vector3.Dot(n, Light.d1)) + Light.c2 * MathF.Max(0, Vector3.Dot(n, Light.d2));
            return Vector3.Min(c, Vector3.One);
        }

        void Tri(int i0, int i1, int i2)
        {
            if (i0 >= vbuf.Length || i1 >= vbuf.Length || i2 >= vbuf.Length) return;
            bool lit = (geom & 0x00020000) != 0;
            int t = texTile, t1 = (texTile + 1) & 7;
            var (tex, tlut) = texOn ? TileTexture(t) : (null, null);
            var (tex1, tlut1) = texOn ? TileTexture(t1) : (null, null);
            // per corner: the combiner's weight of each texture, and what is added
            var ks = new (Vector4 k0, Vector4 k1, Vector4 m)[3];
            int[] idx = { i0, i1, i2 };
            for (int k = 0; k < 3; k++)
            {
                var v = vbuf[idx[k]];
                var shade = lit ? new Vector4(Lit(v.r, v.g, v.b), v.al / 255f) : new Vector4(v.r / 255f, v.g / 255f, v.b / 255f, v.al / 255f);
                ks[k] = Combine(shade);
            }
            bool use0 = tex != null && ks.Any(q => Lum(q.k0) > 0.01f);
            bool use1 = tex1 != null && ks.Any(q => Lum(q.k1) > 0.01f);
            void Swap()
            {
                (t, t1) = (t1, t); (tex, tex1) = (tex1, tex); (tlut, tlut1) = (tlut1, tlut);
                for (int k = 0; k < 3; k++) ks[k] = (ks[k].k1, ks[k].k0, ks[k].m);
            }
            if (use1 && !use0) { Swap(); use0 = true; use1 = false; }
            // (the same texture drawn the same way by both: one)
            bool same = tex1 == tex && tlut1 == tlut && tileShiftS[t1] == tileShiftS[t] && tileShiftT[t1] == tileShiftT[t] && tileUls[t1] == tileUls[t] && tileUlt[t1] == tileUlt[t];
            float mix = 0;
            if (use0 && use1)
            {
                mix = ks.Average(q => Lum(q.k1) / Math.Max(1e-4f, Lum(q.k0) + Lum(q.k1)));
                mix = MathF.Round(mix * 8) / 8;
                if (mix >= 1 && !same) Swap();
                if (same || mix <= 0 || mix >= 1) use1 = false;
            }
            bool texAlpha = use0 && ks.Any(q => MathF.Abs(q.k0.W) + MathF.Abs(q.k1.W) > 0.01f);
            var (_, tw, th, _) = use0 ? TexInfo(tex) : (0, 1, 1, (byte[])null);
            Material key;
            float pw = tw, ph = th;
            if (use0 && use1)
            {
                var (_, w1, h1, _) = TexInfo(tex1);
                float As = Shift(1, tileShiftS[t1]) / Shift(1, tileShiftS[t]), At = Shift(1, tileShiftT[t1]) / Shift(1, tileShiftT[t]);
                float Os = tileUls[t] * As - tileUls[t1], Ot = tileUlt[t] * At - tileUlt[t1];
                static int Period(int w0, int cm0, int w1_, int cm1, float a)
                {
                    if ((cm0 & 2) != 0) return w0;  // (clamped: the texture once)
                    int p0 = (cm0 & 1) != 0 ? 2 * w0 : w0;
                    int p1 = (int)MathF.Round(((cm1 & 1) != 0 ? 2 * w1_ : w1_) / a);
                    return Math.Clamp(Math.Max(p0, p1), 1, 1024);
                }
                pw = Period(tw, tileCms[t], w1, tileCms[t1], As);
                ph = Period(th, tileCmt[t], h1, tileCmt[t1], At);
                key = new Material(tex, tlut, tileCms[t], tileCmt[t], texAlpha, tex1, tlut1, tileCms[t1], tileCmt[t1], mix, As, At, Os, Ot, (int)pw, (int)ph);
            }
            else key = new Material(use0 ? tex : null, use0 ? tlut : null, use0 ? tileCms[t] : 0, use0 ? tileCmt[t] : 0, texAlpha);
            var corners = new Corner[3];
            // A decal (the render mode's depth mode 3: OoT's paths and marks drawn on the ground in its plane, the N64's
            // depth test letting them through): lifted off what it lies on, as there is no such depth mode here.
            var lift = Vector3.Zero;
            if ((otherL >> 10 & 3) == 3)
            {
                var n = Vector3.Cross(vbuf[i1].p - vbuf[i0].p, vbuf[i2].p - vbuf[i0].p);
                if (n.LengthSquared() > 1e-6f) lift = Vector3.Normalize(n) * 3f;
            }
            for (int k = 0; k < 3; k++)
            {
                var v = vbuf[idx[k]];
                v.p += lift;
                var (k0, k1, m) = ks[k];
                var kk = k0 + k1;
                // the colour that multiplies the texture (or, untextured, the colour itself)
                var col = use0 ? new Vector4(kk.X, kk.Y, kk.Z, texAlpha ? kk.W : m.W) : m;
                float s = v.s / 32f * texS, tq = v.t / 32f * texT;
                s = Shift(s, tileShiftS[t]) - tileUls[t];
                tq = Shift(tq, tileShiftT[t]) - tileUlt[t];
                corners[k] = new Corner(v.p, new Vector2(s / pw, tq / ph), Vector4.Clamp(col, Vector4.Zero, Vector4.One));
            }
            if (!Materials.ContainsKey(key)) { Materials[key] = $"m{Materials.Count:D3}"; Combiners[key] = $"cc {combine:X14} oh {otherH:X8} geom {geom:X8} mix {mix}"; }
            Tris.Add((corners[0], corners[1], corners[2], key));
            TriPart.Add(Part);
            // (neither side culled - G_CULL_FRONT 0x200, G_CULL_BACK 0x400 both clear: fences, leaves - seen from both sides)
            if ((geom & 0x600) == 0) { Tris.Add((corners[0], corners[2], corners[1], key)); TriPart.Add(Part); }
        }

        static float Shift(float v, int sh) => sh == 0 ? v : sh <= 10 ? v / (1 << sh) : v * (1 << (16 - sh));

        readonly Dictionary<string, (int, int, int, byte[])> texInfo = new();
        (int, int, int, byte[]) TexInfo(string tex)
        {
            if (!texInfo.TryGetValue(tex, out var ti))
            {
                var (type, w, h, _) = ReadTexture(A, tex);
                texInfo[tex] = ti = (type, Math.Max(1, w), Math.Max(1, h), null);
            }
            return ti;
        }
    }

    // A room's display lists, from its mesh command (opaque first, then the translucent).
    static List<string> RoomLists(O2rArchive a, string room)
    {
        var d = a.Read(room);
        var lists = new List<string>();
        if (d == null) return lists;
        // Its commands, in order, each its id then its own fields: the mesh (0x0A) found by trying each place it could be.
        for (int at = 4; at + 8 < d.Length; at++)
        {
            if (BitConverter.ToUInt32(d, at) != 0x0A) continue;
            try
            {
                var r = new ResReader(d) { Pos = at + 4 };
                r.S8();
                int type = r.S8();
                if (type is not (0 or 2)) continue;
                int num = r.S8();
                if (num <= 0) continue;
                var found = new List<string>();
                var xlu = new List<string>();
                for (int i = 0; i < num; i++)
                {
                    r.S8();
                    if (type == 2) { r.S16(); r.S16(); r.S16(); r.S16(); }
                    string o = r.Str(), x = r.Str();
                    if (o.Length > 0) found.Add(o);
                    if (x.Length > 0) xlu.Add(x);
                }
                if (found.Concat(xlu).All(a.Has) && found.Count + xlu.Count > 0) { lists.AddRange(found); lists.AddRange(xlu); return lists; }
            }
            catch (Exception) { }
        }
        return lists;
    }

    public static void Import(string o2r, string sceneName, string outDir, float scale, Action<string> log)
    {
        using var a = new O2rArchive(o2r);
        var spec = ImportScenes(a, new List<(string, Matrix4x4)> { (sceneName, Matrix4x4.Identity) }, sceneName, outDir, scale, log);
        File.WriteAllText(Path.Combine(outDir, "map.json"), JsonSerializer.Serialize(spec, new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        log($"map: {Path.Combine(outDir, "map.json")}");
    }

    public static List<(Vector3, float)> StartPositionsOf(byte[] d) => StartPositions(d);

    // What some scenes' draw code (z_scene_table.c's draw configs) puts in a segment for their display lists to use: the
    // day's windows of Kakariko's houses and Lon Lon Ranch's.
    static readonly Dictionary<string, Dictionary<int, string>> SceneSegmentTextures = new()
    {
        ["spot01"] = new() { [8] = "scenes/shared/spot01_scene/gKakarikoVillageDayWindowTex" },
        ["spot20"] = new() { [8] = "scenes/shared/spot20_scene/gLonLonRanchDayWindowTex" },
    };

    // A scene's light settings by day (its command 0x0F: a count, then 22 bytes each - ambient colour, light 1 direction and
    // colour, light 2 direction and colour, fog colour, fog near/far): ambient and the two lights. A light with no direction
    // (0, 0, 0: the sun's, set by the time of day) is the sun high in the sky, the other from below it.
    static (Vector3 amb, Vector3 d1, Vector3 c1, Vector3 d2, Vector3 c2) SceneLight(byte[] d, (Vector3, Vector3, Vector3, Vector3, Vector3) fallback)
    {
        if (d == null) return fallback;
        for (int at = 4; at + 8 + 22 <= d.Length; at++)
        {
            if (BitConverter.ToUInt32(d, at) != 0x0F) continue;
            uint n = BitConverter.ToUInt32(d, at + 4);
            if (n is 0 or > 32 || at + 8 + n * 22 > d.Length) continue;
            int e = at + 8 + (n >= 4 ? 22 : 0);  // (outdoors, four by the time of day - dawn, day, dusk, night: the day's)
            Vector3 Col(int o) => new Vector3(d[e + o], d[e + o + 1], d[e + o + 2]) / 255f;
            Vector3 Dir(int o) => new((sbyte)d[e + o], (sbyte)d[e + o + 1], (sbyte)d[e + o + 2]);
            var sun = Vector3.Normalize(new Vector3(0.4f, 0.8f, 0.45f));
            var d1 = Dir(3); var d2 = Dir(9);
            d1 = d1.LengthSquared() < 1 ? sun : Vector3.Normalize(d1);
            d2 = d2.LengthSquared() < 1 ? -sun : Vector3.Normalize(d2);
            return (Col(0), d1, Col(6), d2, Col(12));
        }
        return fallback;
    }

    // Scenes drawn and collided into one model (each placed by its transform, in OoT units): the model (glTF), its
    // collision (OBJ), the main scene's first start position as the spawns, OoT's sky. Returns the map's description.
    public static Dictionary<string, object> ImportScenes(O2rArchive a, List<(string name, Matrix4x4 xf)> parts, string mainName, string outDir, float scale, Action<string> log)
    {
        string sceneName = mainName;
        Directory.CreateDirectory(outDir);
        var rsp = new Rsp { A = a, Log = log };
        var actorColl = new List<(string path, Matrix4x4 xf)>();
        var collParts = new List<(string path, Matrix4x4 xf)>();
        var scenery = new Scenery(a, rsp, actorColl, log);
        byte[] sceneData = null;
        foreach (var (partName, partXf) in parts)
        {
        string dir = a.Names.Where(n => n.EndsWith($"/{partName}_scene/{partName}_scene")).Select(n => n[..n.LastIndexOf('/')]).FirstOrDefault()
                     ?? throw new Exception($"no scene {partName}");
        if (partName == mainName) sceneData = a.Read($"{dir}/{partName}_scene");
        var collPath0 = a.Names.FirstOrDefault(n => n.StartsWith($"{dir}/{partName}_sceneCollisionHeader_"));
        if (collPath0 != null) collParts.Add((collPath0, partXf));
        var rooms = a.Names.Where(n => n.StartsWith(dir + "/") && System.Text.RegularExpressions.Regex.IsMatch(n[(dir.Length + 1)..], $@"^{partName}_room_\d+$")).OrderBy(n => n).ToList();
        log($"{dir}: {rooms.Count} rooms");
        foreach (var room in rooms)
        {
            var lists = RoomLists(a, room);
            log($"  {room[(dir.Length + 1)..]}: {lists.Count} display lists");
            rsp.Xf = partXf;
            rsp.SegTextures = SceneSegmentTextures.GetValueOrDefault(partName);
            rsp.Part = partName;
            rsp.Light = SceneLight(a.Read($"{dir}/{partName}_scene"), rsp.Light);
            foreach (var l in lists) rsp.Run(l);
        }
        // the scenery that is an actor in the game (trees, bushes, rocks, signs, fences, the castle's drawbridge...): each
        // drawn where its room puts it, as its draw does (SohActors.cs)
        scenery.PlaceAll(partName, dir, rooms, partXf);
        rsp.Xf = Matrix4x4.Identity;
        }
        scenery.Report();
        if (rsp.Missing > 0) log($"  ({rsp.Missing} references not found)");
        log($"{rsp.Tris.Count} triangles ({scenery.Triangles} of them the actors'), {rsp.Materials.Count} materials");
        foreach (var (m, name) in rsp.Materials.Take(60)) log($"  {name}: {m.Tex?[(m.Tex.LastIndexOf('/') + 1)..]} {m.Tlut?[(m.Tlut.LastIndexOf('/') + 1)..]} cm {m.Cms}/{m.Cmt} alpha {m.TexAlpha} x{rsp.Tris.Count(t => t.m == m)} {rsp.Combiners.GetValueOrDefault(m)}");
        log("  commands: " + string.Join(" ", rsp.Ops.OrderBy(o => o.Key).Select(o => $"{o.Key:X2}:{o.Value}")));

        // ---- the model (COLLADA, with its textures as PNG beside it) ----
        var scene = new IOScene();
        var model = new IOModel { Name = sceneName };
        scene.Models.Add(model);
        foreach (var (m, name) in rsp.Materials)
        {
            var mat = new IOMaterial { Name = name };
            if (m.Tex != null)
            {
                using var img = m.Tex1 == null ? Decode(a, m.Tex, m.Tlut, m.TexAlpha) : Bake(a, m);
                if (img != null)
                {
                    var file = $"{name}.png";
                    img.SaveAsPng(Path.Combine(outDir, file));
                    static WrapMode Wrap(int cm) => (cm & 2) != 0 ? WrapMode.CLAMP : (cm & 1) != 0 ? WrapMode.MIRROR : WrapMode.REPEAT;
                    // (a baked pair repeats over its period: anything mirrored already unfolded in it)
                    mat.DiffuseMap = m.Tex1 == null ? new IOTexture { Name = name, FilePath = file, WrapS = Wrap(m.Cms), WrapT = Wrap(m.Cmt) }
                                                    : new IOTexture { Name = name, FilePath = file, WrapS = Wrap(m.Cms & 2), WrapT = Wrap(m.Cmt & 2) };
                }
            }
            scene.Materials.Add(mat);
        }
        // a mesh per scene (each place of the map drawn as its own: the stage shows only the camera's)
        foreach (var part in rsp.TriPart.Distinct())
        {
            var mesh = new IOMesh { Name = part.Length > 0 ? part : sceneName };
            model.Meshes.Add(mesh);
            foreach (var g in rsp.Tris.Where((t, i) => rsp.TriPart[i] == part).GroupBy(t => t.m))
            {
                var poly = new IOPolygon { PrimitiveType = IOPrimitive.TRIANGLE, MaterialName = rsp.Materials[g.Key] };
                foreach (var (c0, c1, c2, _) in g)
                    foreach (var c in new[] { c0, c1, c2 })
                    {
                        var v = new IOVertex { Position = c.P * scale };
                        v.SetUV(c.UV.X, 1 - c.UV.Y, 0);  // (V up, as the format's)
                        v.SetColor(c.C.X, c.C.Y, c.C.Z, c.C.W, 0);
                        poly.Indicies.Add(mesh.Vertices.Count);
                        mesh.Vertices.Add(v);
                    }
                mesh.Polygons.Add(poly);
            }
        }
        var modelPath = Path.Combine(outDir, $"{sceneName}.gltf");
        GltfIO.Write(scene, modelPath);
        log($"model: {modelPath}");

        // ---- the collision (the scene's own, as the game's) ----
        string collFile = null;
        if (collParts.Count > 0)
        {
            var sb = new StringBuilder();
            int nvAll = 0, kept = 0;
            void Add(string path, Matrix4x4 xf)
            {
                var r = new ResReader(a.Read(path));
                for (int i = 0; i < 6; i++) r.S16();
                int nv = r.S32();
                var verts = new Vector3[nv];
                for (int i = 0; i < nv; i++) verts[i] = Vector3.Transform(new Vector3(r.S16(), r.S16(), r.S16()), xf) * scale;
                int np = (int)r.U32();
                foreach (var v in verts) sb.Append($"v {v.X:R} {v.Y:R} {v.Z:R}\n");
                // (the turn alone: an actor's placement can be scaled)
                Matrix4x4.Decompose(xf, out _, out var turn, out _);
                var nxf = Matrix4x4.CreateFromQuaternion(turn);
                for (int i = 0; i < np; i++)
                {
                    r.U16();
                    int ia = r.U16() & 0x1FFF, ib = r.U16() & 0x1FFF, ic = r.U16() & 0x1FFF;
                    var n = Vector3.TransformNormal(new Vector3(r.S16(), r.S16(), r.S16()) / 32767f, nxf);
                    r.S16();
                    if (ia >= nv || ib >= nv || ic >= nv) continue;
                    // (wound so its normal is the polygon's)
                    var cn = Vector3.Cross(verts[ib] - verts[ia], verts[ic] - verts[ia]);
                    if (Vector3.Dot(cn, n) < 0) (ib, ic) = (ic, ib);
                    sb.Append($"f {nvAll + ia + 1} {nvAll + ib + 1} {nvAll + ic + 1}\n");
                    kept++;
                }
                nvAll += nv;
            }
            foreach (var (path, xf) in collParts) Add(path, xf);
            int sceneKept = kept;
            foreach (var (path, xf) in actorColl) if (a.Has(path)) Add(path, xf);
            int actorKept = kept - sceneKept;
            // the solids round trees, boulders, signs and the like (each already wound outward)
            foreach (var (p0, p1, p2) in scenery.Solids)
            {
                foreach (var v in new[] { p0 * scale, p1 * scale, p2 * scale }) sb.Append($"v {v.X:R} {v.Y:R} {v.Z:R}\n");
                sb.Append($"f {nvAll + 1} {nvAll + 2} {nvAll + 3}\n");
                nvAll += 3;
                kept++;
            }
            collFile = $"{sceneName}_collision.obj";
            File.WriteAllText(Path.Combine(outDir, collFile), sb.ToString());
            log($"collision: {nvAll} vertices, {kept} triangles (the scenes' {sceneKept}, actors' own {actorKept}, solids round scenery {scenery.Solids.Count})");
        }

        // ---- the riders' spawns: the scene's first start position, the four side by side ----
        var spawns = new List<Placed>();
        var starts = StartPositions(sceneData);
        if (starts.Count > 0)
        {
            var (p, yawDeg) = starts[0];
            float r2 = yawDeg * MathF.PI / 180f;
            var side = new Vector3(MathF.Cos(r2), 0, -MathF.Sin(r2));
            for (int i = 0; i < 4; i++)
            {
                var q = p * scale + side * ((i - 1.5f) * 12f) + new Vector3(0, 10, 0);
                // (Link's facing is +Z at yaw 0; the stage's start positions face -Z at yaw 0)
                spawns.Add(new Placed { X = q.X, Y = q.Y, Z = q.Z, Yaw = yawDeg + 180 });
            }
            log($"spawns: start position 0 at ({p.X}, {p.Y}, {p.Z})");
        }
        var spec = new Dictionary<string, object>
        {
            ["name"] = sceneName == "spot00" ? "Hyrule Field" : sceneName,
            ["model"] = Path.GetFileName(modelPath),
            ["scale"] = 1.0,
            ["spawns"] = spawns,
            ["itemSpots"] = 300,
        };
        if (collFile != null) spec["collision"] = collFile;
        // ---- the sky: OoT's day sky (four sides round the horizon, and overhead), as a panorama and a top ----
        if (a.Has("textures/vr_fine1_static/gDaySkybox1Tex"))
        {
            const string sky = "textures/vr_fine1_static/gDaySkybox", tlut = "textures/vr_fine1_pal_static/gDaySkyboxTLUT";
            var sides = Enumerable.Range(1, 4).Select(i => Decode(a, $"{sky}{i}Tex", tlut, false)).ToList();
            foreach (var sd in sides) if (sd != null) FillDarkEdges(sd);
            if (sides.All(x => x != null))
            {
                using var pano = new Image<Rgba32>(sides.Sum(x => x.Width), sides.Max(x => x.Height));
                int x0 = 0;
                foreach (var sd in sides) { pano.Mutate(c => c.DrawImage(sd, new SixLabors.ImageSharp.Point(x0, 0), 1f)); x0 += sd.Width; sd.Dispose(); }
                pano.SaveAsPng(Path.Combine(outDir, "sky_panorama.png"));
                spec["skyPanorama"] = "sky_panorama.png";
            }
            using var top = Decode(a, $"{sky}5Tex", tlut, false);
            if (top != null) FillDarkEdges(top);
            if (top != null) { top.SaveAsPng(Path.Combine(outDir, "sky_top.png")); spec["skyTop"] = "sky_top.png"; }
            log("sky: OoT's day sky");
        }
        return spec;
    }

    // A sky picture's edge columns and rows that are black (OoT's sky textures: their last column is not the sky's - OoT
    // never draws it, the next face's first takes its place): each the colour of the one inside it.
    static void FillDarkEdges(Image<Rgba32> im)
    {
        bool Dark(Rgba32 p) => p.R + p.G + p.B < 60;
        bool DarkColumn(int x) { int n = 0; for (int y = 0; y < im.Height; y++) if (Dark(im[x, y])) n++; return n > im.Height / 2; }
        bool DarkRow(int y) { int n = 0; for (int x = 0; x < im.Width; x++) if (Dark(im[x, y])) n++; return n > im.Width / 2; }
        for (int x = im.Width - 1; x > im.Width / 2 && DarkColumn(x); x--) for (int y = 0; y < im.Height; y++) im[x, y] = im[x - 1, y];
        for (int x = 0; x < im.Width / 2 && DarkColumn(x); x++) for (int y = 0; y < im.Height; y++) im[x, y] = im[x + 1, y];
        for (int y = im.Height - 1; y > im.Height / 2 && DarkRow(y); y--) for (int x = 0; x < im.Width; x++) im[x, y] = im[x, y - 1];
        for (int y = 0; y < im.Height / 2 && DarkRow(y); y++) for (int x = 0; x < im.Width; x++) im[x, y] = im[x, y + 1];
        // (and single dark pixels left: their neighbour's)
        for (int y = 0; y < im.Height; y++) for (int x = 0; x < im.Width; x++) if (Dark(im[x, y])) im[x, y] = im[x > 0 ? x - 1 : x + 1, y];
    }

    // An actor's placement (OoT's: translate, then turn about Y, X, Z; angles in 1/65536 of a turn).
    static Matrix4x4 Place(Vector3 pos, Vector3 rot)
    {
        static float R(float a) => a * MathF.PI * 2 / 65536f;
        return Matrix4x4.CreateRotationZ(R(rot.Z)) * Matrix4x4.CreateRotationX(R(rot.X)) * Matrix4x4.CreateRotationY(R(rot.Y)) * Matrix4x4.CreateTranslation(pos);
    }

    // A room's actors (its command 0x01: a count, then 16-byte entries - id, position, turn, parameters).
    static List<(int id, Vector3 pos, Vector3 rot, int prm)> RoomActors(O2rArchive a, string room)
    {
        var d = a.Read(room);
        var list = new List<(int, Vector3, Vector3, int)>();
        if (d == null) return list;
        for (int at = 4; at + 8 < d.Length; at++)
        {
            if (BitConverter.ToUInt32(d, at) != 0x01) continue;
            uint n = BitConverter.ToUInt32(d, at + 4);
            if (n == 0 || n > 512 || at + 8 + n * 16 > d.Length) continue;
            bool ok = true;
            for (int i = 0; i < n && ok; i++) ok = BitConverter.ToUInt16(d, at + 8 + i * 16) is > 0 and < 0x1D7;
            if (!ok) continue;
            for (int i = 0; i < n; i++)
            {
                int e = at + 8 + i * 16;
                short S(int o) => BitConverter.ToInt16(d, e + o);
                list.Add((BitConverter.ToUInt16(d, e), new Vector3(S(2), S(4), S(6)), new Vector3(S(8), S(10), S(12)), BitConverter.ToUInt16(d, e + 14)));
            }
            return list;
        }
        return list;
    }

    // The scene's start positions (its command 0x00): Link's, an actor entry each (position, facing).
    static List<(Vector3, float)> StartPositions(byte[] d)
    {
        var list = new List<(Vector3, float)>();
        // (found as the mesh is: an id, a count, then 16-byte entries of actor 0, Link)
        for (int at = 4; at + 8 < d.Length; at++)
        {
            if (BitConverter.ToUInt32(d, at) != 0x00) continue;
            uint n = BitConverter.ToUInt32(d, at + 4);
            if (n == 0 || n > 64 || at + 8 + n * 16 > d.Length) continue;
            bool ok = true;
            for (int i = 0; i < n && ok; i++) ok = BitConverter.ToInt16(d, at + 8 + i * 16) == 0;
            if (!ok) continue;
            for (int i = 0; i < n; i++)
            {
                int e = at + 8 + i * 16;
                var p = new Vector3(BitConverter.ToInt16(d, e + 2), BitConverter.ToInt16(d, e + 4), BitConverter.ToInt16(d, e + 6));
                float yaw = BitConverter.ToInt16(d, e + 10) * 360f / 65536f;
                list.Add((p, yaw));
            }
            return list;
        }
        return list;
    }
}
