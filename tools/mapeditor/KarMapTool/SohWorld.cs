// OoT's scenes around one (Hyrule Field and the places its exits lead to) as one City Trial map: each scene drawn and
// collided as SohScene does, the neighbours set out round the main one (each beyond it, the way its exit faces), and
// OoT's own exits - its collision's exit polygons - made the map's warps (invisible) between them, each landing where
// OoT puts Link coming in that way (the entrance table: tools/mapeditor/KarMapTool/oot_entrances.txt). Each neighbour is
// an area of the map with its own music (its OoT sequence, rendered to a sound file: music/<NNN>.wav when there).
using System.Numerics;
using System.Text.Json;

public static class SohWorld
{
    // The places taken in, as far as exits lead from the main scene through them (the rest - grottos, houses, shops,
    // dungeons - are not): Hyrule's outdoors, its towns, and the Gerudos' hideout.
    static readonly HashSet<string> Neighbours = new()
    {
        "spot01", "spot02", "spot03", "spot04", "spot05", "spot06", "spot07", "spot08", "spot09", "spot10", "spot12",
        "spot15", "spot16", "spot17", "spot18", "spot20", "entra", "market_day", "gerudoway",
    };
    static readonly Dictionary<string, string> Names = new()
    {
        ["spot00"] = "Hyrule Field", ["spot01"] = "Kakariko Village", ["spot02"] = "Kakariko Graveyard", ["spot03"] = "Zora's River",
        ["spot04"] = "Kokiri Forest", ["spot05"] = "Sacred Forest Meadow", ["spot06"] = "Lake Hylia", ["spot07"] = "Zora's Domain",
        ["spot08"] = "Zora's Fountain", ["spot09"] = "Gerudo Valley", ["spot10"] = "Lost Woods", ["spot12"] = "Gerudo's Fortress",
        ["spot15"] = "Hyrule Castle", ["spot16"] = "Death Mountain Trail", ["spot17"] = "Death Mountain Crater", ["spot18"] = "Goron City",
        ["spot20"] = "Lon Lon Ranch", ["entra"] = "Market Entrance", ["market_day"] = "Castle Town Market", ["gerudoway"] = "Thieves' Hideout",
    };

    public class Scene
    {
        public string Name, Dir;
        public byte[] Data;
        public List<(Vector3 p, float yaw)> Starts = new();
        public List<int> SpawnStart = new();   // an entrance's spawn number -> its start position
        public List<int> Exits = new();        // exit number - 1 -> the entrance it takes you to
        public int Seq = -1;                   // its music (OoT's sequence number)
        public Vector3[] Verts = Array.Empty<Vector3>();
        public List<(int a, int b, int c, int exit)> Polys = new();
        public Vector3 Lo, Hi;
        public Vector3 Offset;                 // where it is set in the map (OoT units, before the scale)
    }

    static Dictionary<int, (string scene, int spawn)> Entrances(string toolDir)
    {
        var d = new Dictionary<int, (string, int)>();
        var path = Path.Combine(toolDir, "oot_entrances.txt");
        if (!File.Exists(path)) return d;
        foreach (var l in File.ReadLines(path))
        {
            if (l.StartsWith("#")) continue;
            var f = l.Split(' ');
            if (f.Length >= 3) d[int.Parse(f[0])] = (f[1], int.Parse(f[2]));
        }
        return d;
    }

    public static Scene Load(O2rArchive a, string name)
    {
        string dir = a.Names.Where(n => n.EndsWith($"/{name}_scene/{name}_scene")).Select(n => n[..n.LastIndexOf('/')]).FirstOrDefault();
        if (dir == null) return null;
        var s = new Scene { Name = name, Dir = dir, Data = a.Read($"{dir}/{name}_scene") };
        var d = s.Data;
        s.Starts = SohScene.StartPositionsOf(d);
        // its commands found by what they hold (see SohScene.RoomLists): an id, a count, then the entries
        for (int at = 4; at + 8 < d.Length; at++)
        {
            uint id = BitConverter.ToUInt32(d, at);
            if (id == 0x13 && s.Exits.Count == 0)  // exits: entrance numbers
            {
                uint n = BitConverter.ToUInt32(d, at + 4);
                if (n is 0 or > 64 || at + 8 + n * 2 > d.Length) continue;
                var l = Enumerable.Range(0, (int)n).Select(i => (int)BitConverter.ToUInt16(d, at + 8 + i * 2)).ToList();
                // (entrance numbers; an unused slot 0 - Kokiri Forest has one - among them)
                if (l.All(e => e < 0x620) && l.Count(e => e > 0) >= Math.Max(1, l.Count / 2)) s.Exits = l;
            }
            else if (id == 0x06 && s.SpawnStart.Count == 0)  // entrances: (start position, room) each
            {
                uint n = BitConverter.ToUInt32(d, at + 4);
                if (n is 0 or > 64 || at + 8 + n * 2 > d.Length) continue;
                var l = Enumerable.Range(0, (int)n).Select(i => (int)d[at + 8 + i * 2]).ToList();
                if (l.All(x => x < Math.Max(1, s.Starts.Count)) && Enumerable.Range(0, (int)n).All(i => d[at + 9 + i * 2] < 32)) s.SpawnStart = l;
            }
            else if (id == 0x15 && s.Seq < 0 && at + 7 <= d.Length && d[at + 4] < 8 && d[at + 6] < 0x70 && d[at + 7] < 0x20)  // sound: reverb, ambience, music
                s.Seq = d[at + 6];
        }
        // (music OoT's code starts there rather than the scene: the Lost Woods' Saria's Song)
        if (s.Seq < 0 && name == "spot10") s.Seq = 62;
        // its collision: the polygons, and each one's exit (its surface type: bits 8-12 of its second word)
        var collPath = a.Names.FirstOrDefault(n => n.StartsWith($"{dir}/{name}_sceneCollisionHeader_"));
        if (collPath != null)
        {
            var r = new ResReader(a.Read(collPath));
            for (int i = 0; i < 6; i++) r.S16();
            int nv = r.S32();
            s.Verts = new Vector3[nv];
            for (int i = 0; i < nv; i++) s.Verts[i] = new Vector3(r.S16(), r.S16(), r.S16());
            int np = (int)r.U32();
            var raw = new List<(int type, int a, int b, int c, Vector3 n)>();
            for (int i = 0; i < np; i++)
            {
                int type = r.U16();
                int ia = r.U16() & 0x1FFF, ib = r.U16() & 0x1FFF, ic = r.U16() & 0x1FFF;
                var n = new Vector3(r.S16(), r.S16(), r.S16()) / 32767f;
                r.S16();
                raw.Add((type, ia, ib, ic, n));
            }
            int ns = (int)r.U32();
            var exitOf = new int[ns];
            for (int i = 0; i < ns; i++) { r.U32(); uint w0 = r.U32(); exitOf[i] = (int)(w0 >> 8 & 0x1F); }
            foreach (var (type, ia, ib, ic, n) in raw)
            {
                if (ia >= nv || ib >= nv || ic >= nv) continue;
                int b = ib, c = ic;
                if (Vector3.Dot(Vector3.Cross(s.Verts[ib] - s.Verts[ia], s.Verts[ic] - s.Verts[ia]), n) < 0) (b, c) = (c, b);
                s.Polys.Add((ia, b, c, type < ns ? exitOf[type] : 0));
            }
            s.Lo = s.Verts.Aggregate(new Vector3(float.MaxValue), Vector3.Min);
            s.Hi = s.Verts.Aggregate(new Vector3(float.MinValue), Vector3.Max);
        }
        return s;
    }

    public static void Import(string o2r, string mainName, string outDir, float scale, Action<string> log)
    {
        using var a = new O2rArchive(o2r);
        var entrances = Entrances(AppContext.BaseDirectory);
        if (entrances.Count == 0) entrances = Entrances(Path.GetDirectoryName(typeof(SohWorld).Assembly.Location));
        var main = Load(a, mainName) ?? throw new Exception($"no scene {mainName}");
        log($"{mainName}: {main.Polys.Count} polygons, {main.Exits.Count} exits, {main.Starts.Count} start positions, music {main.Seq}");
        var scenes = new List<Scene> { main };
        // the neighbours: where the main scene's exits lead (the places taken in)
        var exitWhere = new Dictionary<(Scene, int), Vector3>();  // (scene, exit number) -> the middle of its polygons
        Vector3 ExitMiddle(Scene s, int e)
        {
            if (exitWhere.TryGetValue((s, e), out var m)) return m;
            var ps = s.Polys.Where(p => p.exit == e).SelectMany(p => new[] { s.Verts[p.a], s.Verts[p.b], s.Verts[p.c] }).ToList();
            m = ps.Count > 0 ? ps.Aggregate(Vector3.Zero, (x, y) => x + y) / ps.Count : Vector3.Zero;
            return exitWhere[(s, e)] = m;
        }
        // (outward from the main scene, place by place: each one set beyond the one whose exit leads to it, the way that
        // exit faces, and below the main scene - its top well under the main scene's lowest point, out of sight from it)
        for (int qi = 0; qi < scenes.Count; qi++)
        {
            var from = scenes[qi];
            for (int e = 1; e <= from.Exits.Count; e++)
            {
                if (!entrances.TryGetValue(from.Exits[e - 1], out var to) || !Neighbours.Contains(to.scene) || scenes.Any(s => s.Name == to.scene)) continue;
                if (!from.Polys.Any(p => p.exit == e)) continue;
                var s = Load(a, to.scene);
                if (s == null) continue;
                var fLo = from.Lo + from.Offset; var fHi = from.Hi + from.Offset;
                var mid = (fLo + fHi) / 2;
                var dir = ExitMiddle(from, e) + from.Offset - mid; dir.Y = 0;
                dir = dir.LengthSquared() > 1 ? Vector3.Normalize(dir) : Vector3.UnitX;
                var half = (s.Hi - s.Lo) / 2;
                var sMid = (s.Lo + s.Hi) / 2;
                float reach = (new Vector2(fHi.X - fLo.X, fHi.Z - fLo.Z).Length() + new Vector2(half.X, half.Z).Length() * 2) / 2 + 400;
                for (int tries = 0; tries < 200; tries++, reach += 600)
                {
                    var c = mid + dir * reach;
                    s.Offset = new Vector3(c.X - sMid.X, main.Lo.Y - s.Hi.Y - 3000, c.Z - sMid.Z);
                    bool overlaps = scenes.Any(o => Overlap(o.Lo + o.Offset, o.Hi + o.Offset, s.Lo + s.Offset, s.Hi + s.Offset, 1500));
                    if (!overlaps) break;
                }
                scenes.Add(s);
                log($"  {Names.GetValueOrDefault(s.Name, s.Name)} ({s.Name}), from {from.Name}: {s.Polys.Count} polygons, {s.Exits.Count} exits, music {s.Seq}, set at ({s.Offset.X:F0}, {s.Offset.Z:F0})");
            }
        }

        // ---- every scene drawn and collided where it is set (SohScene's, with an offset each) ----
        var parts = scenes.Select(s => (s.Name, Matrix4x4.CreateTranslation(s.Offset))).ToList();
        var spec = SohScene.ImportScenes(a, parts, mainName, outDir, scale, log);

        // ---- the warps: each exit polygon set between taken-in scenes, both ways ----
        var warps = new List<Warp>();
        foreach (var s in scenes)
            for (int e = 1; e <= s.Exits.Count; e++)
            {
                if (!entrances.TryGetValue(s.Exits[e - 1], out var to)) continue;
                var dest = scenes.FirstOrDefault(x => x.Name == to.scene);
                if (dest == null || to.spawn >= dest.SpawnStart.Count || dest.SpawnStart[to.spawn] >= dest.Starts.Count) continue;
                var ps = s.Polys.Where(p => p.exit == e).SelectMany(p => new[] { s.Verts[p.a], s.Verts[p.b], s.Verts[p.c] }).ToList();
                if (ps.Count == 0) continue;
                var lo = ps.Aggregate(new Vector3(float.MaxValue), Vector3.Min) + s.Offset;
                var hi = ps.Aggregate(new Vector3(float.MinValue), Vector3.Max) + s.Offset;
                // (a wall of exit polygons is thin: made deep enough to be ridden into; a floor one tall enough)
                var size = Vector3.Max(hi - lo, new Vector3(60, 160, 60)) + new Vector3(40, 0, 40);
                var c = (lo + hi) / 2 + new Vector3(0, 40, 0);
                var (sp, syaw) = dest.Starts[dest.SpawnStart[to.spawn]];
                var tp = (sp + dest.Offset) * scale + new Vector3(0, 10, 0);
                warps.Add(new Warp
                {
                    Name = $"{s.Name}_exit{e}", X = c.X * scale, Y = c.Y * scale, Z = c.Z * scale, Yaw = 0,
                    SizeX = size.X * scale, SizeY = size.Y * scale, SizeZ = size.Z * scale,
                    ToX = tp.X, ToY = tp.Y, ToZ = tp.Z, ToYaw = syaw + 180, Visible = false,
                });
                log($"  warp: {Names.GetValueOrDefault(s.Name, s.Name)} exit {e} -> {Names.GetValueOrDefault(dest.Name, dest.Name)} (its spawn {to.spawn})");
            }
        // ---- the areas: each neighbour, with its music if rendered (music/<NNN>.wav beside the map, or one named for it) ----
        var areas = new List<Area>();
        string Music(Scene s)
        {
            if (s.Seq < 0 || (s != main && s.Seq == main.Seq)) return null;  // (none of its own: the field's goes on, as in OoT)
            var dir = Path.Combine(outDir, "music");
            if (!Directory.Exists(dir)) return null;
            var f = Directory.GetFiles(dir).FirstOrDefault(p => Path.GetFileNameWithoutExtension(p).StartsWith($"{s.Seq:D3}"));
            return f == null ? null : Path.GetRelativePath(outDir, f);
        }
        // (each place an area with a minimap of its own - the main one first, the one shown at the start)
        foreach (var s in scenes)
        {
            var lo = (s.Lo + s.Offset) * scale; var hi = (s.Hi + s.Offset) * scale;
            var c = (lo + hi) / 2; var size = hi - lo + new Vector3(100, 600, 100);
            areas.Add(new Area { Name = s.Name, X = c.X, Y = c.Y, Z = c.Z, SizeX = size.X, SizeY = size.Y, SizeZ = size.Z, Music = s == main ? null : Music(s), Minimap = true });
        }
        spec["warps"] = warps;
        spec["areas"] = areas;
        if (Music(main) is string mm) spec["music"] = mm;
        spec["name"] = Names.GetValueOrDefault(mainName, mainName);
        File.WriteAllText(Path.Combine(outDir, "map.json"), JsonSerializer.Serialize(spec, new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        // (the music each place wants, for rendering: music/<NNN>.wav)
        File.WriteAllLines(Path.Combine(outDir, "music_wanted.txt"), scenes.Where(s => s.Seq >= 0).Select(s => $"{s.Seq:D3} {s.Name} {Names.GetValueOrDefault(s.Name, s.Name)}"));
        log($"map: {scenes.Count} places, {warps.Count} warps, {areas.Count} areas");
    }

    static bool Overlap(Vector3 aLo, Vector3 aHi, Vector3 bLo, Vector3 bHi, float margin) =>
        aLo.X - margin < bHi.X && bLo.X - margin < aHi.X && aLo.Z - margin < bHi.Z && bLo.Z - margin < aHi.Z;
}
