// The City Trial minimap for a map: a picture of it from above in place of the city's (the HUD's IfAll1c.dat and
// IfAll1cs.dat: root ScInfCitymap_scene_models, one square textured 160 x 200), the square made bigger to take in the
// whole map, and the stage's minimap scale (GrCity1.dat, stage node +0x60) made smaller by as much - so it stays the size
// it was on screen and the riders' marks still land where they are.
//
// How the game places a rider's mark (0x8011DCF8, 0x80124890): on the square, at p / 64 + (O + (3, 0, -1)) / (3.2 S),
// p the rider's position (the stage's units x its scale, +0x08), S the minimap scale (+0x60), O its offset (+0x64..+0x6C).
// The city's square spans -15..15 (the picture's 160 pixels across -12..12, 200 down -15..15; row 0 at -Z), S = 0.5.
using HSDRaw;
using HSDRaw.AirRide.Gr;
using HSDRaw.AirRide.Gr.Data;
using HSDRaw.Common;
using HSDRaw.GX;
using IONET.Core;
using IONET.Core.Model;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using System.Numerics;

public static class Minimap
{
    const int W = 160, H = 200;

    // The textures of a scene's materials, by material name.
    public static Dictionary<string, Image<Rgba32>> Textures(IOScene scene, string modelDir)
    {
        var images = new Dictionary<string, Image<Rgba32>>();
        foreach (var mt in scene.Materials)
        {
            var t = mt.DiffuseMap;
            if (t == null) continue;
            try
            {
                if (t.FileBlob != null && t.Type != ImageFileType.NONE) images[mt.Name] = SixLabors.ImageSharp.Image.Load<Rgba32>(t.FileBlob);
                else if (!string.IsNullOrEmpty(t.FilePath))
                {
                    var path = Path.IsPathRooted(t.FilePath) ? t.FilePath : Path.Combine(modelDir, t.FilePath);
                    if (File.Exists(path)) images[mt.Name] = SixLabors.ImageSharp.Image.Load<Rgba32>(path);
                }
            }
            catch (Exception) { }
        }
        return images;
    }

    // The minimaps' layout: one scale for them all (the game's is set once), big enough for the biggest region, each
    // picture centred on its region. No regions: the whole map, centred on the stage's middle (where the map is).
    public static (List<(string name, Vector3 lo, Vector3 hi)> regions, List<Vector2> centres, float k, float halfX, float halfZ) Layout(
        IOModel model, List<(string name, Vector3 lo, Vector3 hi)> regions, float stageScale, float margin)
    {
        if (regions == null || regions.Count == 0)
        {
            var lo = new Vector3(float.MaxValue); var hi = new Vector3(float.MinValue);
            foreach (var m in model.Meshes) foreach (var v in m.Vertices) { lo = Vector3.Min(lo, v.Position); hi = Vector3.Max(hi, v.Position); }
            float rx = Math.Max(Math.Abs(lo.X), Math.Abs(hi.X)), rz = Math.Max(Math.Abs(lo.Z), Math.Abs(hi.Z));
            regions = new() { ("map", new Vector3(-rx, lo.Y, -rz), new Vector3(rx, hi.Y, rz)) };
        }
        var centres = regions.Select(r => new Vector2((r.lo.X + r.hi.X) / 2, (r.lo.Z + r.hi.Z) / 2)).ToList();
        float reachX = regions.Max(r => (r.hi.X - r.lo.X) / 2) * stageScale, reachZ = regions.Max(r => (r.hi.Z - r.lo.Z) / 2) * stageScale;
        float k = Math.Max(reachX / (12 * 64f), reachZ / (15 * 64f)) * Math.Max(1f, margin);  // (the city's picture covers 768 x 960 each side)
        float halfX = 12 * 64f * k / stageScale, halfZ = 15 * 64f * k / stageScale;  // (in the stage file's units)
        return (regions, centres, k, halfX, halfZ);
    }

    // The minimaps as the HUD will show them (the editor's preview): one per region, or the whole map's.
    public static List<(string name, Image<Rgba32> image)> Preview(IOScene scene, IOModel model, string modelDir,
                                                                   List<(string name, Vector3 lo, Vector3 hi)> regions, float stageScale, MinimapStyle style)
    {
        var (rs, centres, _, halfX, halfZ) = Layout(model, regions, stageScale, style?.Margin ?? 1.04f);
        var images = Textures(scene, modelDir);
        return rs.Select((r, i) => (r.name, Render(model, images, centres[i], halfX, halfZ, rs.Count > 1, r.lo, r.hi, W, H, style, true))).ToList();
    }

    static Vector4 Hex(string s, Vector4 fallback)
    {
        try { var c = System.Drawing.ColorTranslator.FromHtml(s); return new Vector4(c.R / 255f, c.G / 255f, c.B / 255f, 1); }
        catch { return fallback; }
    }

    // The map from above: its highest surface at each point, textured and coloured, w x h pixels over centre +- (halfX,
    // halfZ) (row 0 at -Z). clip: only the triangles whose middle is in lo..hi. hud: as the HUD shows it (brighter, a
    // little see-through, outlined); else plain (the editor's view from above).
    public static Image<Rgba32> Render(IOModel model, Dictionary<string, Image<Rgba32>> images, Vector2 centre, float halfX, float halfZ,
                                       bool clip, Vector3 rlo, Vector3 rhi, int W, int H, MinimapStyle style, bool hud, int SS = 3)
    {
        style ??= new MinimapStyle();
        var land = Hex(style.LandColor, new Vector4(0.5f, 0.7f, 0.44f, 1));
        bool many = clip;
        int w = W * SS, h = H * SS;
        var col = new Vector4[w * h];
        var top = new float[w * h];
        Array.Fill(top, float.MinValue);
        Vector2 ToPx(Vector3 p) => new((p.X - centre.X + halfX) / (2 * halfX) * w, (p.Z - centre.Y + halfZ) / (2 * halfZ) * h);
        foreach (var mesh in model.Meshes)
            foreach (var poly in mesh.Polygons)
            {
                Image<Rgba32> img = null;
                if (style.Textured) images.TryGetValue(poly.MaterialName ?? "", out img);
                for (int i = 0; i + 2 < poly.Indicies.Count; i += 3)
                {
                    var va = mesh.Vertices[poly.Indicies[i]]; var vb = mesh.Vertices[poly.Indicies[i + 1]]; var vc = mesh.Vertices[poly.Indicies[i + 2]];
                    var mid = (va.Position + vb.Position + vc.Position) / 3;
                    if (many && (mid.X < rlo.X || mid.X > rhi.X || mid.Z < rlo.Z || mid.Z > rhi.Z)) continue;
                    Vector2 a = ToPx(va.Position), b = ToPx(vb.Position), c = ToPx(vc.Position);
                    float den = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
                    if (MathF.Abs(den) < 1e-6f) continue;  // (seen edge on from above: a wall)
                    var n = Vector3.Normalize(Vector3.Cross(vb.Position - va.Position, vc.Position - va.Position));
                    float shade = 0.55f + 0.45f * MathF.Abs(n.Y);  // (slopes darker)
                    int x0 = Math.Max(0, (int)MathF.Floor(MathF.Min(a.X, MathF.Min(b.X, c.X)))), x1 = Math.Min(w - 1, (int)MathF.Ceiling(MathF.Max(a.X, MathF.Max(b.X, c.X))));
                    int y0 = Math.Max(0, (int)MathF.Floor(MathF.Min(a.Y, MathF.Min(b.Y, c.Y)))), y1 = Math.Min(h - 1, (int)MathF.Ceiling(MathF.Max(a.Y, MathF.Max(b.Y, c.Y))));
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++)
                        {
                            float px = x + 0.5f, py = y + 0.5f;
                            float w0 = ((b.Y - c.Y) * (px - c.X) + (c.X - b.X) * (py - c.Y)) / den;
                            float w1 = ((c.Y - a.Y) * (px - c.X) + (a.X - c.X) * (py - c.Y)) / den;
                            float w2 = 1 - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                            float y3 = w0 * va.Position.Y + w1 * vb.Position.Y + w2 * vc.Position.Y;
                            int at = y * w + x;
                            if (y3 <= top[at]) continue;
                            var cc = style.Textured ? (va.Colors.Count > 0 ? w0 * va.Colors[0] + w1 * vb.Colors[0] + w2 * vc.Colors[0] : Vector4.One) : land;
                            if (img != null && va.UVs.Count > 0)
                            {
                                var uv = w0 * va.UVs[0] + w1 * vb.UVs[0] + w2 * vc.UVs[0];
                                float u = uv.X - MathF.Floor(uv.X), vv = (1 - uv.Y) - MathF.Floor(1 - uv.Y);  // (V up, as the scene holds it)
                                var tc = img[Math.Clamp((int)(u * img.Width), 0, img.Width - 1), Math.Clamp((int)(vv * img.Height), 0, img.Height - 1)].ToScaledVector4();
                                cc *= tc;
                            }
                            if (cc.W < 0.3f) continue;  // (see-through: what is under it shows)
                            top[at] = y3;
                            col[at] = new Vector4(cc.X * shade, cc.Y * shade, cc.Z * shade, 1);
                        }
                }
            }
        var pic = new Image<Rgba32>(W, H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                var sum = Vector4.Zero;
                for (int sy = 0; sy < SS; sy++) for (int sx = 0; sx < SS; sx++) sum += col[(y * SS + sy) * w + x * SS + sx];
                sum /= SS * SS;
                var rgb = new Vector3(sum.X, sum.Y, sum.Z) / Math.Max(sum.W, 1e-4f);
                if (hud)  // (colours a little brighter and richer, as a map is; empty: clear)
                {
                    rgb = Vector3.Clamp(((rgb - new Vector3(0.5f)) * style.Contrast + new Vector3(0.56f)) * style.Brightness, Vector3.Zero, Vector3.One);
                    pic[x, y] = new Rgba32(new Vector4(rgb, sum.W < 0.5f ? 0 : Math.Clamp(style.Opacity, 0.05f, 1f)));
                }
                else pic[x, y] = new Rgba32(new Vector4(Vector3.Clamp(rgb, Vector3.Zero, Vector3.One), sum.W < 0.5f ? 0 : 1));
            }
        if (!hud || !style.Outline) return pic;
        // an outline round the land, so it reads on the HUD
        var edgeCol = Hex(style.OutlineColor, Vector4.One);
        var outlined = pic.Clone();
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                if (pic[x, y].A != 0) continue;
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; dy++) for (int dx = -1; dx <= 1 && !edge; dx++)
                {
                    int xx = x + dx, yy = y + dy;
                    edge = xx >= 0 && yy >= 0 && xx < W && yy < H && pic[xx, yy].A != 0;
                }
                if (edge) outlined[x, y] = new Rgba32(new Vector4(edgeCol.X, edgeCol.Y, edgeCol.Z, 0.9f));
            }
        pic.Dispose();
        return outlined;
    }

    // regions: the parts of the map with a minimap each (the first the one shown at the start; the game shows the one the
    // first rider is in - src/runtime/maps.cpp, minimaps.txt), in the stage file's units; none: the whole map, one minimap.
    public static void Build(IOScene scene, IOModel model, string modelDir, KAR_grStageNode stage, List<(string name, Vector3 lo, Vector3 hi)> regions,
                             string baseDir, string outDir, Action<string> log, MinimapStyle style = null)
    {
        float stageScale = stage.StageScale;
        var (rs, centres, k, halfX, halfZ) = Layout(model, regions, stageScale, style?.Margin ?? 1.04f);
        regions = rs;
        log($"minimap: {regions.Count} of them, {k:F2} times the city's reach, {halfX * 2:F0} x {halfZ * 2:F0}");
        var images = Textures(scene, modelDir);
        Image<Rgba32> Render(int region) => Minimap.Render(model, images, centres[region], halfX, halfZ, regions.Count > 1, regions[region].lo, regions[region].hi, W, H, style, true);
        using var outlined = Render(0);
        outlined.SaveAsPng(Path.Combine(outDir, "minimap.png"));

        // ---- into the HUD's files: the picture, and the square made k times as big ----
        var bgra = new byte[W * H * 4];
        outlined.CloneAs<Bgra32>().CopyPixelDataTo(bgra);
        foreach (var name in new[] { "IfAll1c.dat", "IfAll1cs.dat" })
        {
            var src = Path.Combine(baseDir, name);
            if (!File.Exists(src)) { log($"  ({name} not found: minimap left as the city's)"); continue; }
            var f = new HSDRawFile(src);
            var root = f.Roots.FirstOrDefault(r => r.Name == "ScInfCitymap_scene_models");
            if (root == null) { log($"  ({name}: no ScInfCitymap_scene_models)"); continue; }
            var arr = new HSDNullPointerArrayAccessor<HSD_JOBJDesc> { _s = root.Data._s };
            var j = arr.Array[0].RootJoint;
            var d = j.Dobj;
            d.Mobj.Textures.EncodeImageData(bgra, W, H, GXTexFmt.CI8, GXTlutFmt.RGB5A3);  // (as the city's: 8-bit, a palette)
            d.Mobj.Textures.MagFilter = GXTexFilter.GX_LINEAR;
            foreach (var p in d.Pobj.List)
            {
                // (its attribute list, as stored: each 0x18 bytes, the position's found by its name)
                var list = p.Attributes._s;
                int ai = Enumerable.Range(0, list.Length / 0x18).First(i => (GXAttribName)list.GetInt32(i * 0x18) == GXAttribName.GX_VA_POS);
                var pa = new GX_Attribute { _s = list.GetEmbeddedStruct(ai * 0x18, 0x18) };
                var buf = pa.Buffer._s.GetData();
                if (pa.AttributeType != GXAttribType.GX_INDEX8 && pa.AttributeType != GXAttribType.GX_INDEX16) throw new Exception("the minimap square's positions are not indexed");
                if (pa.CompType == GXCompType.Float)
                {
                    for (int o = 0; o + 4 <= buf.Length; o += 4)
                    {
                        var v = BitConverter.ToSingle(new[] { buf[o + 3], buf[o + 2], buf[o + 1], buf[o] }, 0) * k;
                        var bytes = BitConverter.GetBytes(v);
                        buf[o] = bytes[3]; buf[o + 1] = bytes[2]; buf[o + 2] = bytes[1]; buf[o + 3] = bytes[0];
                    }
                }
                else
                {
                    // fixed point, 8 or 16 bits: room made for the bigger numbers by a bit less of fraction each doubling
                    bool wide = pa.CompType is GXCompType.Int16 or GXCompType.UInt16, signed = pa.CompType is GXCompType.Int8 or GXCompType.Int16;
                    int size = wide ? 2 : 1, shift = pa.Scale, lose = 0;
                    float limit = (wide ? 32767f : 127f) * (signed ? 1 : 2);
                    while (lose < shift && 15f * k * (1 << (shift - lose)) > limit) lose++;
                    for (int o = 0; o + size <= buf.Length; o += size)
                    {
                        int raw = wide ? (signed ? (short)(buf[o] << 8 | buf[o + 1]) : (buf[o] << 8 | buf[o + 1])) : (signed ? (sbyte)buf[o] : buf[o]);
                        int q = (int)Math.Round(raw / (float)(1 << shift) * k * (1 << (shift - lose)));
                        if (wide) { buf[o] = (byte)(q >> 8); buf[o + 1] = (byte)q; } else buf[o] = (byte)q;
                    }
                    list.SetByte(ai * 0x18 + 0x10, (byte)(shift - lose));
                }
                pa.Buffer._s.SetData(buf);
            }
            f.Save(Path.Combine(outDir, name));
        }
        // ---- the stage's scale and offset: the same size on screen, the window on the first region ----
        // (a mark lands at p / 64 + (O + (3, 0, -1)) / (3.2 S) on the square: a window centred on C takes O = -3.2 S C / 64 - (3, 0, -1))
        float S = 0.5f / k;
        Vector3 Offset(Vector2 c) => new(-3.2f * S * c.X * stageScale / 64f - 3f, 0, -3.2f * S * c.Y * stageScale / 64f + 1f);
        var o0 = Offset(centres[0]);
        stage._s.SetFloat(0x60, S);
        stage._s.SetFloat(0x64, o0.X);
        stage._s.SetFloat(0x68, o0.Y);
        stage._s.SetFloat(0x6C, o0.Z);
        // ---- each region's picture (the game's own encoding: 8-bit, its palette) and offset, for the game to change to ----
        if (regions.Count > 1)
        {
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            string F(float v) => v.ToString("0.###", inv);
            var lines = new List<string> { "# minimap <name> <picture: its pixels then its palette> <offset x y z> <region, world units: lo x z, hi x z>" };
            for (int i = 0; i < regions.Count; i++)
            {
                using var pic = i == 0 ? outlined.Clone() : Render(i);
                var px = new byte[W * H * 4];
                pic.CloneAs<Bgra32>().CopyPixelDataTo(px);
                var enc = HSDRaw.Tools.GXImageConverter.EncodeImage(px, W, H, GXTexFmt.CI8, GXTlutFmt.RGB5A3, out byte[] pal);
                var file = $"minimap_{i}.bin";
                File.WriteAllBytes(Path.Combine(outDir, file), enc.Concat(pal).ToArray());
                if (i > 0) pic.SaveAsPng(Path.Combine(outDir, $"minimap_{i}.png"));
                var o = Offset(centres[i]);
                var (rlo, rhi) = (regions[i].lo * stageScale, regions[i].hi * stageScale);
                lines.Add($"minimap {regions[i].name.Replace(' ', '_')} {file} {F(o.X)} {F(o.Y)} {F(o.Z)} {F(rlo.X)} {F(rlo.Z)} {F(rhi.X)} {F(rhi.Z)} {enc.Length} {pal.Length}");
            }
            File.WriteAllLines(Path.Combine(outDir, "minimaps.txt"), lines);
        }
        foreach (var img in images.Values) img.Dispose();
    }
}
