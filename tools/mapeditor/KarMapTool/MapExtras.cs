// What a map has beyond its ground: its music (in place of City Trial's), its areas (parts with music of their own) and
// its warps. The game finds the areas and warps in mapinfo.txt beside the stage's files (src/runtime/maps.cpp), in the
// stage's world units (the file's x its scale); the music goes in as the game's own tracks (audio/jp/*.hps): the map's
// in City Trial's (city.hps), each area's in a track City Trial never plays.
using IONET.Core;
using IONET.Core.Model;
using System.Globalization;
using System.Numerics;
using System.Text;

public static class MapExtras
{
    // Tracks City Trial never plays (Top Ride's courses, ids 3-0x11 but its results' 2d_kekka): an area's music takes one
    // (src/runtime/maps.cpp knows them by these names).
    public static readonly string[] AreaTracks =
    {
        "2d_desert", "2d_desert_ura", "2d_kazan", "2d_kazan_ura", "2d_light", "2d_light_ura", "2d_machine",
        "2d_machine_ura", "2d_plants", "2d_plants_ura", "2d_sky", "2d_sky_ura", "2d_water", "2d_water_ura",
    };

    public static void Build(MapSpec spec, IOScene scene, IOModel model, Func<Vector3, Vector3> convert, float stageScale, string outDir, Action<string> log)
    {
        var inv = CultureInfo.InvariantCulture;
        string F(float v) => v.ToString("0.###", inv);
        var info = new StringBuilder("# A custom map's areas and warps (KarMapTool): world units (the stage file's x its scale).\n");
        info.Append($"# area <name> <centre x y z> <half size x y z> <yaw> <track>\n# warp <name> <centre x y z> <half size x y z> <yaw> <to x y z> <to yaw>\n");
        var audio = Path.Combine(outDir, "audio", "jp");

        // ---- music ----
        if (spec.Music != null)
        {
            Directory.CreateDirectory(audio);
            var hps = MapMusic.ToHps(spec.Music, spec.MusicLoopStart, log);
            File.WriteAllBytes(Path.Combine(audio, "city.hps"), hps);
            File.WriteAllBytes(Path.Combine(audio, "city_ura.hps"), hps);  // (City Trial's other city music)
        }

        // a box, in world units: its centre, half size, turn
        (Vector3 c, Vector3 h, float yaw) World(Box b)
        {
            var c = convert(b.Position) * stageScale;
            var h = new Vector3(b.SizeX, b.SizeY, b.SizeZ) * 0.5f * spec.Scale * stageScale;
            return (c, h, b.Yaw);
        }
        var named = spec.Areas.Cast<Box>().Concat(spec.Warps).Where(b => !string.IsNullOrEmpty(b.Name)).GroupBy(b => b.Name).ToDictionary(g => g.Key, g => g.First());

        // ---- areas ----
        // (each piece of music one of the game's spare tracks: areas with the same music share it)
        var trackOf = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int n = 0;
        foreach (var a in spec.Areas)
        {
            string name = string.IsNullOrEmpty(a.Name) ? $"area{n}" : a.Name;
            n++;
            string t = "-";
            if (a.Music != null && !trackOf.TryGetValue($"{a.Music}|{a.LoopStart}", out t))
            {
                if (trackOf.Count >= AreaTracks.Length) { log($"  area {a.Name}: no track left for its music ({AreaTracks.Length} pieces at most): the map's plays"); t = "-"; }
                else
                {
                    Directory.CreateDirectory(audio);
                    t = AreaTracks[trackOf.Count];
                    trackOf[$"{a.Music}|{a.LoopStart}"] = t;
                    File.WriteAllBytes(Path.Combine(audio, t + ".hps"), MapMusic.ToHps(a.Music, a.LoopStart, log));
                }
            }
            t ??= "-";
            var (c, h, yaw) = World(a);
            info.Append($"area {name.Replace(' ', '_')} {F(c.X)} {F(c.Y)} {F(c.Z)} {F(h.X)} {F(h.Y)} {F(h.Z)} {F(yaw)} {t}\n");
        }

        // ---- warps ----
        var rings = new List<(Vector3 c, Vector3 h, float yaw)>();
        foreach (var w in spec.Warps)
        {
            var (c, h, yaw) = World(w);
            Vector3 to; float toYaw;
            if (!string.IsNullOrEmpty(w.To) && named.TryGetValue(w.To, out var target))
            {
                // in front of the box it names (out of it, so as not to warp back)
                var (tc, th, ty) = World(target);
                float r = ty * MathF.PI / 180f;
                var fwd = new Vector3(-MathF.Sin(r), 0, -MathF.Cos(r));
                to = tc + fwd * (th.Z + 40 * stageScale);  // (at its middle height: dropped onto the ground)
                toYaw = ty;
            }
            else
            {
                to = convert(new Vector3(w.ToX, w.ToY, w.ToZ)) * stageScale;
                toYaw = w.ToYaw;
            }
            string name = string.IsNullOrEmpty(w.Name) ? $"warp{rings.Count}" : w.Name;
            info.Append($"warp {name.Replace(' ', '_')} {F(c.X)} {F(c.Y)} {F(c.Z)} {F(h.X)} {F(h.Y)} {F(h.Z)} {F(yaw)} {F(to.X)} {F(to.Y)} {F(to.Z)} {F(toYaw)}\n");
            if (w.Visible) rings.Add((convert(w.Position), new Vector3(w.SizeX, w.SizeY, w.SizeZ) * 0.5f * spec.Scale, yaw));
        }
        File.WriteAllText(Path.Combine(outDir, "mapinfo.txt"), info.ToString());
        log($"map info: {spec.Areas.Count} areas ({trackOf.Count} pieces of music), {spec.Warps.Count} warps{(spec.Music != null ? ", its own music" : "")}");

        // ---- the warps' rings: an upright ring at each, across its box, facing its way ----
        if (rings.Count > 0)
        {
            scene.Materials.Add(new IOMaterial { Name = "kar_warp_ring" });
            var mesh = new IOMesh { Name = "kar_warp_rings" };
            var poly = new IOPolygon { PrimitiveType = IOPrimitive.TRIANGLE, MaterialName = "kar_warp_ring" };
            const int seg = 24;
            foreach (var (c, h, yaw) in rings)
            {
                float r = yaw * MathF.PI / 180f;
                var right = new Vector3(MathF.Cos(r), 0, -MathF.Sin(r));
                var fwd = new Vector3(-MathF.Sin(r), 0, -MathF.Cos(r));
                float rx = h.X, ry = h.Y, thick = Math.Max(2f, Math.Min(h.X, h.Y) * 0.12f);
                var mid = c;
                for (int i = 0; i < seg; i++)
                {
                    float a0 = i * MathF.Tau / seg, a1 = (i + 1) * MathF.Tau / seg;
                    Vector3 P(float a, float k) => mid + right * MathF.Cos(a) * (rx - k) + Vector3.UnitY * MathF.Sin(a) * (ry - k);
                    var q = new[] { P(a0, 0), P(a1, 0), P(a1, thick), P(a0, thick) };
                    var col = new Vector4(0.45f + 0.55f * (i % 2), 0.95f, 1f, 1f);
                    // both sides
                    foreach (var tri in new[] { (0, 1, 2), (0, 2, 3), (0, 2, 1), (0, 3, 2) })
                        foreach (int k in new[] { tri.Item1, tri.Item2, tri.Item3 })
                        {
                            var v = new IOVertex { Position = q[k], Normal = fwd };
                            v.SetColor(col.X, col.Y, col.Z, col.W, 0);
                            v.SetUV(0, 0, 0);
                            poly.Indicies.Add(mesh.Vertices.Count);
                            mesh.Vertices.Add(v);
                        }
                }
            }
            mesh.Polygons.Add(poly);
            model.Meshes.Add(mesh);
        }
    }
}
