// Builds a City Trial map's stage files from a MapSpec, on the city's own (from the player's disc): GrCity1.dat (the
// stage's data: collision, its partition tree, positions) and GrCity1Model.dat (what is drawn). The game serves them in
// place of the city's when the map is chosen (src/runtime/hle_dvd.cpp, dvdOverlay).
using HSDRaw;
using HSDRaw.AirRide.Gr;
using HSDRaw.AirRide.Gr.Data;
using HSDRaw.Common;
using HSDRaw.GX;
using HSDRawViewer.Converters;
using HSDRawViewer.Tools.SpatialOrganizer;
using IONET;
using IONET.Core;
using IONET.Core.Model;
using SixLabors.ImageSharp.Processing;
using System.ComponentModel;
using System.Numerics;

public static class MapBuilder
{
    // A triangle of the map, in stage units.
    record struct Tri(Vector3 A, Vector3 B, Vector3 C)
    {
        public Vector3 Normal => Vector3.Normalize(Vector3.Cross(B - A, C - A));
        public float Area => Vector3.Cross(B - A, C - A).Length() * 0.5f;
    }

    public static void Build(MapSpec spec, string baseDir, string outDir, Action<string> log)
    {
        Directory.CreateDirectory(outDir);
        log($"importing {spec.Model}");
        // the objects placed in the model's file (Blender's empties): spawns, areas, warps - added to the map's own
        foreach (var mk in GltfIO.Markers(spec.Model))
        {
            string kind = mk.Name[4..].ToLowerInvariant(), dir = Path.GetDirectoryName(spec.Model);
            string Prop(string k) => mk.Extras.TryGetPropertyValue(k, out var v) && v != null ? v.ToString() : null;
            float PropF(string k) => float.TryParse(Prop(k), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f) ? f : 0;
            string Rel(string p) => p == null || Path.IsPathRooted(p) ? p : Path.Combine(dir, p);
            string label = mk.Name[4..];
            if (kind.StartsWith("spawn")) spec.Spawns.Add(new Placed { X = mk.Position.X, Y = mk.Position.Y, Z = mk.Position.Z, Yaw = mk.Yaw });
            else if (kind.StartsWith("area"))
                spec.Areas.Add(new Area { Name = label, X = mk.Position.X, Y = mk.Position.Y, Z = mk.Position.Z, Yaw = mk.Yaw, SizeX = mk.Size.X, SizeY = mk.Size.Y, SizeZ = mk.Size.Z, Music = Rel(Prop("music")), LoopStart = PropF("loop_start") });
            else if (kind.StartsWith("warp"))
                spec.Warps.Add(new Warp { Name = label, X = mk.Position.X, Y = mk.Position.Y, Z = mk.Position.Z, Yaw = mk.Yaw, SizeX = mk.Size.X, SizeY = mk.Size.Y, SizeZ = mk.Size.Z, To = Prop("to") is string t ? (t.StartsWith("KAR_", StringComparison.OrdinalIgnoreCase) ? t[4..] : t) : null, Visible = Prop("visible") != "0" && Prop("visible") != "false" });
            else { log($"  {mk.Name}: not a kind of object the map has (KAR_Spawn, KAR_Area, KAR_Warp)"); continue; }
            log($"  object {mk.Name}");
        }
        var scene = GltfIO.Load(spec.Model);
        if (scene.Models.Count == 0) throw new Exception("the model file has no model in it");
        var model = scene.Models[0];
        // to stage units, Y up - and centred on the stage's origin (across; heights as they are), where its sky is
        var offset = Vector3.Zero;
        Vector3 Convert(Vector3 p) => (spec.ZUp ? new Vector3(p.X, p.Z, -p.Y) : p) * spec.Scale + offset;
        foreach (var mesh in model.Meshes)
        {
            mesh.MakeTriangles();
            foreach (var v in mesh.Vertices)
            {
                v.Position = Convert(v.Position);
                if (spec.ZUp) v.Normal = new Vector3(v.Normal.X, v.Normal.Z, -v.Normal.Y);
            }
        }
        if (spec.Center)
        {
            var lo = new Vector3(float.MaxValue); var hi = new Vector3(float.MinValue);
            foreach (var mesh in model.Meshes) foreach (var v in mesh.Vertices) { lo = Vector3.Min(lo, v.Position); hi = Vector3.Max(hi, v.Position); }
            var shift = new Vector3(-(lo.X + hi.X) / 2, 0, -(lo.Z + hi.Z) / 2);
            foreach (var mesh in model.Meshes) foreach (var v in mesh.Vertices) v.Position += shift;
            offset = shift;  // (the collision and what is placed: the same)
            log($"centred: moved by ({shift.X:F0}, {shift.Z:F0})");
        }
        spec.Spawns = spec.Spawns.Select(sp => { var q = Convert(sp.Position); return new Placed { X = q.X, Y = q.Y, Z = q.Z, Yaw = sp.Yaw }; }).ToList();
        spec.EventPoints = spec.EventPoints.Select(ep => { var q = Convert(ep.Position); return new EventPoint { X = q.X, Y = q.Y, Z = q.Z, Yaw = ep.Yaw, Event = ep.Event }; }).ToList();
        // normals, where the model has none (an OBJ without vn): each vertex the average of its triangles' (lit by the stage)
        foreach (var mesh in model.Meshes)
        {
            if (mesh.Vertices.Any(v => v.Normal.LengthSquared() > 0.01f)) continue;
            foreach (var poly in mesh.Polygons)
                for (int i = 0; i + 2 < poly.Indicies.Count; i += 3)
                {
                    var a = mesh.Vertices[poly.Indicies[i]]; var b2 = mesh.Vertices[poly.Indicies[i + 1]]; var c2 = mesh.Vertices[poly.Indicies[i + 2]];
                    var n = Vector3.Cross(b2.Position - a.Position, c2.Position - a.Position);
                    a.Normal += n; b2.Normal += n; c2.Normal += n;
                }
            foreach (var v in mesh.Vertices) if (v.Normal.LengthSquared() > 0) v.Normal = Vector3.Normalize(v.Normal);
        }
        var tris = Triangles(model);
        // what is ridden on: the model's own triangles, or a collision model's (the model is then only drawn)
        if (spec.Collision != null)
        {
            log($"importing the collision {spec.Collision}");
            var cs = GltfIO.Load(spec.Collision);
            if (cs.Models.Count == 0) throw new Exception("the collision file has no model in it");
            foreach (var mesh in cs.Models[0].Meshes)
            {
                mesh.MakeTriangles();
                foreach (var v in mesh.Vertices) v.Position = Convert(v.Position);
            }
            tris = Triangles(cs.Models[0]);
        }
        if (tris.Count == 0) throw new Exception("the model has no triangles");
        var min = new Vector3(float.MaxValue);
        var max = new Vector3(float.MinValue);
        foreach (var t in tris)
            foreach (var p in new[] { t.A, t.B, t.C }) { min = Vector3.Min(min, p); max = Vector3.Max(max, p); }
        log($"{tris.Count} triangles, bounds ({min.X:F0}, {min.Y:F0}, {min.Z:F0})..({max.X:F0}, {max.Y:F0}, {max.Z:F0})");
        if (tris.Count > 60000) throw new Exception($"{tris.Count} triangles is too many for the collision (60000 at most): simplify the model");

        // ---- the stage's data (GrCity1.dat) ----
        var data = new HSDRawFile(Path.Combine(baseDir, "GrCity1.dat"));
        var gd = (KAR_grData)data.Roots[0].Data;
        // the map's model hangs from the city's root after its joints (below): its joint's number, the collision's bone
        int mapBone = new HSDRawFile(Path.Combine(baseDir, "GrCity1Model.dat")).Roots.Select(r => r.Data).OfType<KAR_grModel>().First().MainModel.RootNode.TreeList.Count;
        if (spec.ModelOnly || spec.OnCityRoot) mapBone = 0;
        // Collision in small triangles, as the city's (29604 of them): the stage's collision tree sorts triangles into
        // boxes, and a big one (an old game's: a field's ground triangle can span a thousand units) lands in every box it
        // crosses - each query then tests far more of them (800E2FA0 most of the game's time, the frames dropping).
        // (30000 at most: the tree's bit table is sized by a signed 16-bit count - see +0x58 below)
        var collTris = Subdivide(tris, 30000, log);
        var coll = MakeCollision(collTris, mapBone);
        if (spec.RegenCityTree) {  // (debugging: the city's own collision, its tree made again by the generator)
            var ct = SpatialPartitionOrganizer.GeneratePartition(gd.CollisionNode);
            ct.ZoneIndexType = 5; ct.ZoneIndices = new ushort[4096]; ct.RoughTriangleType = 5; ct.RoughIndices = new ushort[4096];
            ct._s.SetInt16(0x58, (short)Math.Max((int)ct.CollidableTriangleCount, 30000));
            gd.PartitionNode.Partition = ct;
            log($"city collision, tree regenerated: {ct.BucketsCount} buckets");
        }
        if (!spec.KeepCityCollision) {
        if (spec.KeepCityZones) { var oc = gd.CollisionNode; coll.ZoneVertices = oc.ZoneVertices; coll.ZoneTriangles = oc.ZoneTriangles; coll.ZoneJoints = oc.ZoneJoints; }
        gd.CollisionNode = coll;
        var tree = SpatialPartitionOrganizer.GeneratePartition(coll);
        // Every object the tree takes - triangles, zones, the course's spline segments - is numbered below its bit table's
        // size (+0x58): the city's is 29604 (its triangle count), the generator's this map's triangle count, below the
        // city's course splines' segments. Its table (+0x54: a bit per object, size/8 + 1 bytes, zeroed) is in the file,
        // the game never makes one: without it every search's marks are written at address 0 (the console's own values
        // there - its bus clock - zeroed: the frame timer stops at its next setting, the game freezes). The size is read
        // as a signed 16-bit number: the triangles' count (the numbers the marks use), never past 32767.
        if (coll.TriangleCount > 32000) throw new Exception($"{coll.TriangleCount} collision triangles: 32000 at most");
        int marks = Math.Max(coll.TriangleCount, 30000);
        tree._s.SetInt16(0x58, (short)marks);
        tree._s.SetReferenceStruct(0x54, new HSDStruct(new byte[((marks >> 3) + 1 + 31) & ~31]));
        gd.PartitionNode.Partition = tree;
        log($"collision: {coll.VertexCount} vertices, {coll.TriangleCount} triangles; partition: {gd.PartitionNode.Partition.BucketsCount} buckets");
        }
        // what belongs to the city alone: its rails (its gimmicks' kinds stay: City Trial asks for them by kind)
        if (gd.RailCollNode != null) gd.RailCollNode.Count = 0;
        // ... and its conveyors', rails' and heavy curves. (The course and range splines stay: City Trial needs them.)
        if (gd.SplineNode is { } sn)
        {
            if (sn.ConveyorSpline?.SplineList != null) sn.ConveyorSpline.SplineList.Count = 0;
            if (sn.RailSpline1 != null) sn.RailSpline1.Count = 0;
            if (sn.HeavySplines != null) sn.HeavySplines.Count = 0;
        }
        // ... and the city's course and roads (kept: City Trial needs them), stretched over the map: the game finds each
        // rider's nearest road (800E2FA0) by walking the collision tree's boxes outward from the rider, so with no road near
        // (the city's, left where they were) it walks a great many. (Never shrunk much: road ends within a unit are joined,
        // and two that meet assert.)
        if (gd.SplineNode is { } sn2)
        {
            var roads = new List<HSD_Spline>();
            var seen = new HashSet<HSDStruct>();
            void Add(HSD_Spline s) { if (s != null && seen.Add(s._s)) roads.Add(s); }
            var course = sn2.SplineSetup?.CourseSplineList;
            if (course != null) foreach (var s in course.Splines.Array) Add(s);
            var range = sn2.RangeSplineSetup;
            if (range != null) foreach (var r in range.Splines) { Add(r.LeftSpline); Add(r.RightSpline); }
            var cmin = new Vector3(float.MaxValue); var cmax = new Vector3(float.MinValue);
            foreach (var s in roads) foreach (var p in s.CV) { var v = new Vector3(p.X, p.Y, p.Z); cmin = Vector3.Min(cmin, v); cmax = Vector3.Max(cmax, v); }
            if (roads.Count > 0)
            {
                float sx = (max.X - min.X) / Math.Max(1, cmax.X - cmin.X), sz = (max.Z - min.Z) / Math.Max(1, cmax.Z - cmin.Z);
                float sy = Math.Min(1, (sx + sz) / 2);
                foreach (var s in roads)
                {
                    var cv = s.CV;
                    foreach (var p in cv) { p.X = min.X + (p.X - cmin.X) * sx; p.Y = min.Y + (p.Y - cmin.Y) * sy; p.Z = min.Z + (p.Z - cmin.Z) * sz; }
                    s.CV = cv;
                    s.TotalLength *= (sx + sz) / 2;
                }
                log($"the city's course and roads: {roads.Count} curves stretched over the map ({sx:F2} x {sz:F2})");
            }
        }
        // the stage's limits: out of bounds a little past the map
        var st = gd.StageNode;
        st.OoBMinXArea = min.X - 200; st.OoBMaxXArea = max.X + 200;
        st.OoBMinZArea = min.Z - 200; st.OoBMaxZArea = max.Z + 200;
        st.OoBMinYArea = min.Y - 300; st.OoBMaxYArea = max.Y + 1500;
        // City Trial's events (GrCity1Event.dat): each one's points, a range of the stage's event list (below)
        var evPath = Path.Combine(baseDir, "GrCity1Event.dat");
        var ev = File.Exists(evPath) ? new HSDRawFile(evPath) : null;
        var evRoot = ev?.Roots.FirstOrDefault(r => r.Name == "grEventDataAllCity1");
        var evParams = evRoot == null ? null : new KAR_grEventDataAllCity1 { _s = evRoot.Data._s }.EventNode?.EventParams;
        if (!spec.KeepCityPositions) {
        Positions(gd.PositionNode, spec, tris, evParams, log);
        }
        // the fog: off, or as the map has it
        if (gd.FogNode?.FogData?.Fog is HSD_FogDesc fog)
        {
            // (off: the fog pushed out past the far plane, its kind kept - City Trial's fog event brings its own in from it)
            if (!spec.Fog.Enabled) { fog.Start = 1e6f; fog.End = 2e6f; log("fog: off (out of sight)"); }
            else
            {
                if (spec.Fog.Start is float fs) fog.Start = fs;
                if (spec.Fog.End is float fe) fog.End = fe;
                if (!string.IsNullOrEmpty(spec.Fog.Color)) fog.Color = System.Drawing.ColorTranslator.FromHtml(spec.Fog.Color);
                log($"fog: {fog.Start:F0} to {fog.End:F0}");
            }
        }
        // (The skies' own fogs - FogTypes, one per sky: the stage changes to one when an event changes the sky, and back to the
        // city's usual, 210 to 665, after - stay: the sky is sized by them. With the fog off the game leaves out all but the
        // Fog event's thick one as it draws: "fog off" in mapinfo.txt, src/runtime/maps.cpp.)
        // the minimap: the map from above, in the HUD (its files beside the stage's), and the stage's scale for it
        var regions = spec.Areas.Where(a => a.Minimap).Select(a =>
        {
            var c = Convert(a.Position); var h = new Vector3(a.SizeX, a.SizeY, a.SizeZ) * 0.5f * spec.Scale;
            return (name: a.Name ?? "area", lo: c - h, hi: c + h);
        }).ToList();
        if (!spec.KeepCityModel) Minimap.Build(scene, model, Path.GetDirectoryName(spec.Model), gd.StageNode, regions, baseDir, outDir, log, spec.MinimapStyle);
        // the map's music, its areas and warps (for the game: mapinfo.txt), the warps' rings (drawn with the map)
        MapExtras.Build(spec, scene, model, Convert, gd.StageNode.StageScale, outDir, log);
        data.Save(Path.Combine(outDir, "GrCity1.dat"));
        // City Trial's events, those that need the city itself never chosen: Rail Fire (6: the city's train and rails),
        // the Lighthouse (8: the city's lighthouse - without it the game stops), the Secret Chamber (9: a room at the
        // city's place). GrCity1Event.dat: grEventDataAllCity1 +0 -> +0 (the config) +2C -> the chances, 8 groups x 16.
        if (ev != null)
        {
            var chances = evRoot?.Data._s.GetReference<HSDAccessor>(0)?._s.GetReference<HSDAccessor>(0)?._s.GetReference<HSDAccessor>(0x2C);
            if (chances != null && chances._s.Length >= 8 * 64)
            {
                foreach (int g in Enumerable.Range(0, 8)) foreach (int e in new[] { 6, 8, 9 }) chances._s.SetInt32(g * 64 + e * 4, 0);
                log("events: the city's own (rail fire, lighthouse, secret chamber) left out");
            }
            else log("  (GrCity1Event.dat not as expected: the city's own events left in)");
            ev.Save(Path.Combine(outDir, "GrCity1Event.dat"));  // (with the events' point ranges, as the stage's list now has them)
        }

        // ---- what is drawn (GrCity1Model.dat): the model in place of the city's, the city's sky kept ----
        if (spec.KeepCityModel) { File.Copy(Path.Combine(baseDir, "GrCity1Model.dat"), Path.Combine(outDir, "GrCity1Model.dat"), true); return; }
        log("converting the model");
        // Textures by file: read here and handed over as images (the importer's own file branch accepts none - it takes the
        // whole path for the extension); any format ImageSharp reads, as PNG.
        foreach (var mt in scene.Materials)
        {
            var tex = mt.DiffuseMap;
            if (tex == null || tex.Type != ImageFileType.NONE || string.IsNullOrEmpty(tex.FilePath)) continue;
            var path = Path.IsPathRooted(tex.FilePath) ? tex.FilePath : Path.Combine(Path.GetDirectoryName(spec.Model), tex.FilePath);
            if (!File.Exists(path)) path = Path.Combine(Path.GetDirectoryName(spec.Model), Path.GetFileName(tex.FilePath));
            if (!File.Exists(path)) { log($"  material {mt.Name}: texture {tex.FilePath} not found"); continue; }
            using var img = SixLabors.ImageSharp.Image.Load(path);
            using var ms = new MemoryStream();
            SixLabors.ImageSharp.ImageExtensions.SaveAsPng(img, ms);
            tex.FileBlob = ms.ToArray();
            tex.Type = ImageFileType.PNG;
        }
        var imp = new ModelImporter(Path.GetDirectoryName(spec.Model), scene, model, new ModelImportSettings { VertexColorFormat = GXCompTypeClr.RGBA8 },
                                    model.Meshes.Select(m => new MeshImportSettings(m) { ImportNormals = ImportNormalSettings.Yes, ImportVertexColor = m.HasColorSet(0) }).ToList(),
                                    scene.Materials.Select(m => new MaterialImportSettings(m) { EnableDiffuse = false, EnableConstant = true, TextureFormat = GXTexFmt.RGB5A3 }).ToList(), null);  // (unlit, as the city's surfaces; textures 16-bit: lossless for OoT's and most old games', half the bytes for the renderer to look at each frame)
        imp.Work(new BackgroundWorker { WorkerReportsProgress = true });
        // (the importer keeps its result to itself: HSDRawViewer's own dialog reads it from inside)
        var jobj = typeof(ModelImporter).GetField("NewModel", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)?.GetValue(imp) as HSD_JOBJ
                   ?? throw new Exception("the model could not be converted");
        // filtered smoothly (as the N64 did), not nearest
        foreach (var jt in jobj.TreeList)
            for (var d = jt.Dobj; d != null; d = d.Next)
            {
                for (var t = d.Mobj?.Textures; t != null; t = t.Next)
                {
                    t.MagFilter = GXTexFilter.GX_LINEAR;
                    t.LOD = new HSD_TOBJ_LOD { MinFilter = GXTexFilter.GX_LINEAR, Bias = 0 };
                }
                // vertex colours as the city's own meshes take them: VERTEX alone, the alpha theirs too (the importer's
                // BOTH - vertex and material - the game draws with the material's alone: trees' leaves, paths came out white)
                if (d.Mobj != null && (d.Mobj.RenderFlags & RENDER_MODE.BOTH) == RENDER_MODE.BOTH)
                    d.Mobj.RenderFlags = (d.Mobj.RenderFlags & ~(RENDER_MODE.CONSTANT | RENDER_MODE.ALPHA_BOTH)) | RENDER_MODE.ALPHA_VTX;
                // See-through (a texture with alpha: leaves, fences): cut out, as the N64 drew them (its alpha compare) -
                // drawn solid where the texture is, left out where it is clear. (The stage's see-through pass drew trees'
                // leaves white, blended or not.)
                if (d.Mobj != null && (d.Mobj.RenderFlags & RENDER_MODE.XLU) != 0)
                {
                    d.Mobj.RenderFlags &= ~RENDER_MODE.XLU;
                    d.Mobj.PEDesc = new HSD_PEDesc { Flags = PIXEL_PROCESS_ENABLE.COLOR_UPDATE | PIXEL_PROCESS_ENABLE.COMPARE | PIXEL_PROCESS_ENABLE.ZUPDATE, AlphaRef0 = 100, DestinationAlpha = 0xFF,
                        BlendMode = GXBlendMode.GX_NONE, SrcFactor = GXBlendFactor.GX_BL_ONE, DstFactor = GXBlendFactor.GX_BL_ZERO, BlendOp = GXLogicOp.GX_LO_NOOP,
                        DepthFunction = GXCompareType.LEqual, AlphaComp0 = GXCompareType.Greater, AlphaOp = GXAlphaOp.And, AlphaComp1 = GXCompareType.Always };
                }
            }
        var modelFile = new HSDRawFile(Path.Combine(baseDir, "GrCity1Model.dat"));
        var gm = (KAR_grModel)modelFile.Roots.First(r => r.Data is KAR_grModel).Data;
        var mm = gm.MainModel;
        // The city's joints stay, as they are, hidden (the game finds its markers - its joints with nothing to draw - its
        // cameras and parts among them by number), and the map's model hangs from the root after them.
        var city = mm.RootNode;
        int cityMeshes = 0;
        // (each of its meshes kept, with no geometry: a joint with a mesh is not a marker)
        foreach (var j in city.TreeList)
            for (var d = j.Dobj; d != null; d = d.Next)
            {
                // (never drawn: its textures dropped too - the stage's memory has no room for the city's and the map's)
                if (!spec.KeepCityMeshes) { d.Pobj = null; if (d.Mobj != null) { d.Mobj.Textures = null; d.Mobj.RenderFlags &= ~(RENDER_MODE.TEX0 | RENDER_MODE.TEX1 | RENDER_MODE.TEX2 | RENDER_MODE.TEX3 | RENDER_MODE.TEX4 | RENDER_MODE.TEX5 | RENDER_MODE.TEX6 | RENDER_MODE.TEX7); } }
                cityMeshes++;
            }
        if (spec.ModelOnly) { mm.RootNode = city = jobj; cityMeshes = 0; }
        // The map's meshes on the city's root joint, after the city's own there: the city's material animations (its
        // root's 77, texture swaps and scrolls) go to meshes by their order on the joint - in front, the map's took them.
        int mapFirst = 0, mapCount = 0;
        if (!spec.ModelOnly && spec.OnCityRoot)
        {
            var dl = jobj.TreeList.SelectMany(x => { var l = new List<HSD_DOBJ>(); for (var d = x.Dobj; d != null; d = d.Next) l.Add(d); return l; }).ToList();
            for (int i = 0; i + 1 < dl.Count; i++) dl[i].Next = dl[i + 1];
            dl[^1].Next = null;
            if (city.Dobj == null) city.Dobj = dl[0];
            else
            {
                var last = city.Dobj; mapFirst = 1;
                while (last.Next != null) { last = last.Next; mapFirst++; }
                last.Next = dl[0];
            }
            mapCount = dl.Count;
            cityMeshes = 0;
        }
        else if (!spec.ModelOnly) {
        var last = city.Child;
        while (last.Next != null) last = last.Next;
        last.Next = jobj;
        }
        // The sky: with parts (places far apart), one round each part - the city's sky, its pictures the map's, centred on the
        // part and big enough to stand round it - drawn with the part. (One sky round them all would stand past what the
        // camera draws.) Without parts: the city's, grown round the map.
        Sky(gm, spec, min, max, log);
        var partSkies = new Dictionary<HSD_JOBJ, int>();  // (each part's sky: its joint -> the part's number in regions)
        if (regions.Count > 1 && gm.SkyboxModel?.JOBJRoot is { } skyRoot && !spec.ModelOnly)
        {
            for (int ri = 0; ri < regions.Count; ri++)
            {
                var (_, lo, hi) = regions[ri];
                var mid = (lo + hi) / 2;
                // (the city's: 2800 from its middle, 1211 to its cap: never smaller, and tall enough for the part's
                // heights; src/runtime/maps.cpp has the camera see as far as across it - the same sizes, from mapinfo.txt)
                float radius = Math.Max(2800f, new Vector2(hi.X - lo.X, hi.Z - lo.Z).Length() / 2 * 1.15f);
                var s = HSDAccessor.DeepClone<HSD_JOBJ>(skyRoot);
                s.Next = null;
                s.TX = mid.X; s.TY = lo.Y - 200; s.TZ = mid.Z;
                s.SX = s.SZ = radius / 2800f;
                s.SY = Math.Max(s.SX, (hi.Y - lo.Y + 1500) / 1211f);
                // (the sky's own model is drawn first, with no depth test; with the map's meshes, after them: tested, so
                // anything nearer stays in front - it never writes depth, being the farthest)
                foreach (var sj in s.TreeList)
                    for (var d = sj.Dobj; d != null; d = d.Next)
                        if (d.Mobj?.PEDesc is { } pe) { pe.Flags |= PIXEL_PROCESS_ENABLE.COMPARE; pe.Flags &= ~PIXEL_PROCESS_ENABLE.ZUPDATE; pe.DepthFunction = GXCompareType.LEqual; }
                var lastChild = city.Child;
                if (lastChild == null) city.Child = s;
                else { while (lastChild.Next != null) lastChild = lastChild.Next; lastChild.Next = s; }
                partSkies[s] = ri;
            }
            skyRoot.SX = skyRoot.SY = skyRoot.SZ = 0.001f;  // (the city's own sky, at the middle of the stage: out of sight)
            log($"sky: one round each of the {regions.Count} parts");
        }
        int joints = 0, dobjs = 0, pobjs = 0;
        foreach (var j in city.TreeList)
        {
            joints++;
            for (var d = j.Dobj; d != null; d = d.Next)
            {
                dobjs++;
                for (var p = d.Pobj; p != null; p = p.Next) pobjs++;
            }
        }
        mm.JobjCount = joints; mm.DobjCount = dobjs; mm.PobjCount = pobjs;
        // What is drawn (KAR's culling, 800E63D4: every mesh hidden, then shown if in the always-drawn list (+18), or in a
        // view region the camera sees - with src/runtime/maps.cpp, one the camera is in - and big enough on screen by its
        // static box (one a mesh, in the tree's order)). A map with parts (areas with minimaps of their own): a view region
        // each, its meshes alone in it, so the camera's part is drawn and not the others far off. Otherwise: all drawn.
        var b = mm.ModelBounding;
        bool IsMap(int i) => mapCount > 0 ? i >= mapFirst && i < mapFirst + mapCount : i >= cityMeshes;
        var all = new List<HSD_DOBJ>();
        foreach (var j in city.TreeList) for (var d = j.Dobj; d != null; d = d.Next) all.Add(d);
        var skyPart = new Dictionary<HSDStruct, int>();  // (a part's sky's meshes -> the part)
        foreach (var (sj, ri) in partSkies) foreach (var j in sj.TreeList) for (var d = j.Dobj; d != null; d = d.Next) skyPart[d._s] = ri;
        int SkyOf(int i) => skyPart.TryGetValue(all[i]._s, out int ri) ? ri : -1;
        var hidden = new KAR_grStaticBoundingBox { MinX = -1, MinY = -9001, MinZ = -1, MaxX = 1, MaxY = -9000, MaxZ = 1 };
        var boxes = new KAR_grStaticBoundingBox[dobjs];
        var mids = new Vector3[dobjs];
        for (int i = 0; i < dobjs; i++)
        {
            if (SkyOf(i) is int sri and >= 0)
            {
                var (_, slo, shi) = regions[sri];
                boxes[i] = new KAR_grStaticBoundingBox { MinX = slo.X, MinY = slo.Y, MinZ = slo.Z, MaxX = shi.X, MaxY = shi.Y, MaxZ = shi.Z };
                continue;
            }
            if (!IsMap(i)) { boxes[i] = hidden; continue; }
            var lo = new Vector3(float.MaxValue); var hi = new Vector3(float.MinValue);
            for (var p = all[i].Pobj; p != null; p = p.Next)
                foreach (var v in p.ToDisplayList().Vertices) { var q = new Vector3(v.POS.X, v.POS.Y, v.POS.Z); lo = Vector3.Min(lo, q); hi = Vector3.Max(hi, q); }
            if (lo.X > hi.X) { lo = min; hi = max; }
            boxes[i] = new KAR_grStaticBoundingBox { MinX = lo.X, MinY = lo.Y, MinZ = lo.Z, MaxX = hi.X, MaxY = hi.Y, MaxZ = hi.Z };
            mids[i] = (lo + hi) / 2;
        }
        b.StaticBoundingBoxes = boxes;
        b.DynamicBoundingBoxes = Array.Empty<KAR_grDynamicBoundingBoxes>();
        var mapIdx = Enumerable.Range(0, dobjs).Where(i => IsMap(i) && SkyOf(i) < 0).ToList();
        if (regions.Count > 1)
        {
            bool In((string name, Vector3 lo, Vector3 hi) r, Vector3 p) => p.X >= r.lo.X && p.X <= r.hi.X && p.Z >= r.lo.Z && p.Z <= r.hi.Z;
            var vr = new List<KAR_grViewRegion>();
            for (int ri = 0; ri < regions.Count; ri++)
            {
                var r = regions[ri];
                var idx = mapIdx.Where(i => In(r, mids[i])).Concat(Enumerable.Range(0, dobjs).Where(i => SkyOf(i) == ri)).Select(i => (ushort)i).ToArray();
                log($"  part {r.name}: {idx.Length} meshes, x {r.lo.X:F0}..{r.hi.X:F0} z {r.lo.Z:F0}..{r.hi.Z:F0}");
                if (idx.Length == 0) continue;
                // (where the camera can be while in it: the part's box, roomy)
                vr.Add(new KAR_grViewRegion { MinX = r.lo.X - 500, MinY = r.lo.Y - 600, MinZ = r.lo.Z - 500, MaxX = r.hi.X + 500, MaxY = r.hi.Y + 2000, MaxZ = r.hi.Z + 500, Indices = idx });
            }
            b.ViewRegions = vr.ToArray();
            // (what is in no part: always drawn)
            b.Indices = mapIdx.Where(i => !regions.Any(r => In(r, mids[i]))).Select(i => (ushort)i).ToArray();
            log($"culling: {vr.Count} parts, each drawn only from inside it; {b.Indices.Length} meshes always drawn");
        }
        else
        {
            b.ViewRegions = new[] { new KAR_grViewRegion { MinX = min.X - 1000, MinY = min.Y - 1000, MinZ = min.Z - 1000, MaxX = max.X + 1000, MaxY = max.Y + 2000, MaxZ = max.Z + 1000, Indices = new ushort[] { 0 } } };
            b.Indices = mapIdx.Select(i => (ushort)i).ToArray();  // (the meshes drawn: the map's)
        }
        // (the city's animations move the city's joints, which draw nothing: they stay)
        modelFile.Save(Path.Combine(outDir, "GrCity1Model.dat"));
        log($"model: {joints} joints, {dobjs} meshes, {pobjs} polygon groups");
    }

    // The sky: the city's (a cylinder in two halves, a cap above), made big enough to stand round the map, and with the
    // map's own pictures if it has them - a panorama all round (the horizon along its bottom) and the sky overhead.
    static void Sky(KAR_grModel gm, MapSpec spec, Vector3 min, Vector3 max, Action<string> log)
    {
        var root = gm.SkyboxModel?.JOBJRoot;
        if (root == null) return;
        // (the city's is 2800 across from the middle: grown to stand well outside the map's farthest corner)
        float reach = new[] { new Vector2(min.X, min.Z), new Vector2(min.X, max.Z), new Vector2(max.X, min.Z), new Vector2(max.X, max.Z) }.Max(c => c.Length());
        float grow = Math.Max(1f, reach * 1.25f / 2800f);
        root.SX = root.SY = root.SZ = grow;
        log($"sky: {grow:F2} times the city's");
        var meshes = new List<HSD_DOBJ>();
        foreach (var j in root.TreeList) for (var d = j.Dobj; d != null; d = d.Next) meshes.Add(d);
        if (meshes.Count < 3) return;
        static byte[] Bgra(SixLabors.ImageSharp.Image<SixLabors.ImageSharp.PixelFormats.Bgra32> im)
        {
            var b = new byte[im.Width * im.Height * 4];
            im.CopyPixelDataTo(b);
            return b;
        }
        void Put(HSD_DOBJ d, SixLabors.ImageSharp.Image<SixLabors.ImageSharp.PixelFormats.Bgra32> im)
        {
            var t = d.Mobj?.Textures;
            if (t == null) return;
            t.EncodeImageData(Bgra(im), im.Width, im.Height, GXTexFmt.RGBA8, GXTlutFmt.RGB565);
            t.MagFilter = GXTexFilter.GX_LINEAR;
        }
        if (spec.SkyPanorama != null && File.Exists(spec.SkyPanorama))
        {
            using var pano = SixLabors.ImageSharp.Image.Load<SixLabors.ImageSharp.PixelFormats.Bgra32>(spec.SkyPanorama);
            pano.Mutate(x => x.Resize(1024, 128, SixLabors.ImageSharp.Processing.KnownResamplers.Bicubic));
            // the halves: the -Z one (mesh 2) from +X round to -X, the +Z one (mesh 1) on round back to +X
            using var a = pano.Clone(x => x.Crop(new SixLabors.ImageSharp.Rectangle(0, 0, 512, 128)));
            using var b = pano.Clone(x => x.Crop(new SixLabors.ImageSharp.Rectangle(512, 0, 512, 128)));
            Put(meshes[2], a);
            Put(meshes[1], b);
            log("sky: the map's panorama");
        }
        if (spec.SkyTop != null && File.Exists(spec.SkyTop))
        {
            using var top = SixLabors.ImageSharp.Image.Load<SixLabors.ImageSharp.PixelFormats.Bgra32>(spec.SkyTop);
            top.Mutate(x => x.Resize(256, 256, SixLabors.ImageSharp.Processing.KnownResamplers.Bicubic));
            Put(meshes[0], top);
            log("sky: the map's sky overhead");
        }
    }

    // Each triangle cut in four (at its edges' middles) while any edge is longer than a limit: the smallest limit (a power
    // of two, from 64) that keeps the count under `most`.
    static List<Tri> Subdivide(List<Tri> tris, int most, Action<string> log)
    {
        List<Tri> Cut(float limit)
        {
            var outp = new List<Tri>();
            var stack = new Stack<Tri>(tris);
            while (stack.Count > 0)
            {
                var t = stack.Pop();
                float e = MathF.Max(Vector3.Distance(t.A, t.B), MathF.Max(Vector3.Distance(t.B, t.C), Vector3.Distance(t.C, t.A)));
                if (e <= limit || outp.Count + stack.Count > most * 4) { outp.Add(t); continue; }
                Vector3 ab = (t.A + t.B) / 2, bc = (t.B + t.C) / 2, ca = (t.C + t.A) / 2;
                stack.Push(new Tri(t.A, ab, ca)); stack.Push(new Tri(ab, t.B, bc)); stack.Push(new Tri(ca, bc, t.C)); stack.Push(new Tri(ab, bc, ca));
            }
            return outp;
        }
        for (float limit = 64; limit < 1e6f; limit *= 2)
        {
            var r = Cut(limit);
            if (r.Count <= most)
            {
                if (r.Count != tris.Count) log($"collision: {tris.Count} triangles cut to {r.Count} (edges {limit:F0} at most)");
                return r;
            }
        }
        return tris;
    }

    static List<Tri> Triangles(IOModel model)
    {
        var tris = new List<Tri>();
        foreach (var mesh in model.Meshes)
            foreach (var poly in mesh.Polygons)
                for (int i = 0; i + 2 < poly.Indicies.Count; i += 3)
                {
                    var t = new Tri(mesh.Vertices[poly.Indicies[i]].Position, mesh.Vertices[poly.Indicies[i + 1]].Position, mesh.Vertices[poly.Indicies[i + 2]].Position);
                    if (t.Area > 1e-6f) tris.Add(t);
                }
        return tris;
    }

    // The map's triangles as the stage's collision: floor, wall or ceiling by how steep each is.
    static KAR_grCollisionNode MakeCollision(List<Tri> tris, int bone)
    {
        var verts = new List<GXVector3>();
        var index = new Dictionary<(int, int, int), int>();
        int Vertex(Vector3 p)
        {
            var key = ((int)MathF.Round(p.X * 64), (int)MathF.Round(p.Y * 64), (int)MathF.Round(p.Z * 64));  // (welded)
            if (!index.TryGetValue(key, out int i))
            {
                i = verts.Count;
                index[key] = i;
                verts.Add(new GXVector3 { X = p.X, Y = p.Y, Z = p.Z });
            }
            return i;
        }
        var triangles = new List<KAR_CollisionTriangle>();
        foreach (var t in tris)
        {
            var n = t.Normal;
            var flag = n.Y >= 0.45f ? KCCollFlag.Floor : n.Y <= -0.45f ? KCCollFlag.Ceiling : KCCollFlag.Wall;
            int a = Vertex(t.A), bb = Vertex(t.B), c = Vertex(t.C);
            if (a == bb || bb == c || a == c) continue;
            // (wound as the game's: the third vertex first)
            triangles.Add(new KAR_CollisionTriangle { V1 = c, V2 = bb, V3 = a, Flags = flag });
        }
        // The stage always has zones (the city's: its pads, its shadows...): one that does nothing, out of reach far below.
        var zv = new List<GXVector3>();
        var zt = new List<KAR_ZoneCollisionTriangle>();
        var zj = new List<KAR_ZoneCollisionJoint>();
        AddZone(zv, zt, zj, new Vector3(0, -5000, 0), new Vector3(2, 2, 2), 0, 23, bone);
        return new KAR_grCollisionNode
        {
            Vertices = verts.ToArray(),
            Triangles = triangles.ToArray(),
            Joints = new[] { new KAR_CollisionJoint { BoneID = bone, VertexStart = 0, VertexSize = verts.Count, FaceStart = 0, FaceSize = triangles.Count, Kind = 3 } },
            ZoneVertices = zv.ToArray(),
            ZoneTriangles = zt.ToArray(),
            ZoneJoints = zj.ToArray(),
        };
    }

    // A zone: a box (8 vertices, 12 triangles, a polygon number per side) of a kind, on the model's root joint.
    static void AddZone(List<GXVector3> zv, List<KAR_ZoneCollisionTriangle> zt, List<KAR_ZoneCollisionJoint> zj, Vector3 centre, Vector3 size, float yawDeg, int kind, int bone)
    {
        int v0 = zv.Count, f0 = zt.Count;
        float r = yawDeg * MathF.PI / 180f, cs = MathF.Cos(r), sn = MathF.Sin(r);
        for (int i = 0; i < 8; i++)
        {
            var o = new Vector3((i & 1) != 0 ? size.X : -size.X, (i & 2) != 0 ? size.Y : -size.Y, (i & 4) != 0 ? size.Z : -size.Z) * 0.5f;
            var p = centre + new Vector3(o.X * cs + o.Z * sn, o.Y, -o.X * sn + o.Z * cs);
            zv.Add(new GXVector3 { X = p.X, Y = p.Y, Z = p.Z });
        }
        // the six sides, outward, each two triangles (wound as the game's collision: see MakeCollision)
        int[][] sides = { new[] { 0, 2, 3, 1 }, new[] { 4, 5, 7, 6 }, new[] { 0, 1, 5, 4 }, new[] { 2, 6, 7, 3 }, new[] { 0, 4, 6, 2 }, new[] { 1, 3, 7, 5 } };
        for (int s = 0; s < 6; s++)
        {
            var q = sides[s];
            zt.Add(new KAR_ZoneCollisionTriangle { PolyIndex = s, V1 = v0 + q[2], V2 = v0 + q[1], V3 = v0 + q[0], Kind = kind });
            zt.Add(new KAR_ZoneCollisionTriangle { PolyIndex = s, V1 = v0 + q[3], V2 = v0 + q[2], V3 = v0 + q[0], Kind = kind });
        }
        zj.Add(new KAR_ZoneCollisionJoint
        {
            BoneID = bone, ZoneVertexStart = v0, ZoneVertexSize = 8, ZoneFaceStart = f0, ZoneFaceSize = 12, ZoneLinkIndex = -1, x18 = 0,
            Mtx00 = cs, Mtx01 = 0, Mtx02 = sn, Mtx10 = 0, Mtx11 = 1, Mtx12 = 0, Mtx20 = -sn, Mtx21 = 0, Mtx22 = cs,
            Mtx30 = centre.X, Mtx31 = centre.Y, Mtx32 = centre.Z,
        });
    }

    // Where riders start, where boxes, power-ups and machines appear, and the like: the city's lists, refilled.
    static void Positions(KAR_grPositionNode pn, MapSpec spec, List<Tri> tris, HSDArrayAccessor<KAR_grEventParam> evParams, Action<string> log)
    {
        var rng = new Random(spec.Seed);
        // points on the floor, spread by area
        var floors = tris.Where(t => t.Normal.Y >= 0.7f).ToList();
        if (floors.Count == 0) throw new Exception("the map has no floor to ride on");
        var cum = new double[floors.Count];
        double total = 0;
        for (int i = 0; i < floors.Count; i++) cum[i] = total += floors[i].Area;
        Vector3 OnFloor(float above)
        {
            int k = Array.BinarySearch(cum, rng.NextDouble() * total);
            if (k < 0) k = ~k;
            var t = floors[Math.Min(k, floors.Count - 1)];
            float u = (float)rng.NextDouble(), v = (float)rng.NextDouble();
            if (u + v > 1) { u = 1 - u; v = 1 - v; }
            return t.A + (t.B - t.A) * u + (t.C - t.A) * v + new Vector3(0, above, 0);
        }
        KAR_grPositionData At(Vector3 p, float yaw = 0)
        {
            float r = yaw * MathF.PI / 180f;
            return new KAR_grPositionData { X = p.X, Y = p.Y, Z = p.Z, FX = -MathF.Sin(r), FY = 0, FZ = -MathF.Cos(r), UX = 0, UY = 1, UZ = 0 };
        }
        // riders: the spawns placed (round them, if fewer than a list holds), else spots on the floor
        var spawns = spec.Spawns.Count > 0 ? spec.Spawns.Select(s => (s.Position, s.Yaw)).ToList()
                                           : Enumerable.Range(0, 4).Select(_ => (OnFloor(20), 0f)).ToList();
        void Fill(HSDArrayAccessor<KAR_grPositionList> lists, Func<int, int, KAR_grPositionData> make)
        {
            if (lists == null) return;
            var arr = lists.Array;
            for (int li = 0; li < arr.Length; li++)
            {
                int n = arr[li].Count;
                if (n == 0) continue;
                arr[li].PositionData = new HSDArrayAccessor<KAR_grPositionData> { Array = Enumerable.Range(0, n).Select(i => make(li, i)).ToArray() };
                arr[li].Count = n;
            }
            lists.Array = arr;
        }
        // The game checks every start position has floor under it (grnullpos.c: "startpos error", then it stops): each one
        // put a little above the floor below it - the highest floor not far above it - else (a spawn under the ground: a
        // tree's foot) on the nearest floor above it, or, with none there, back on its spawn.
        Vector3? Floor(Vector3 p)
        {
            float best = float.MinValue, above = float.MaxValue;
            foreach (var t in floors)
            {
                float den = (t.B.Z - t.C.Z) * (t.A.X - t.C.X) + (t.C.X - t.B.X) * (t.A.Z - t.C.Z);
                if (MathF.Abs(den) < 1e-6f) continue;
                float w0 = ((t.B.Z - t.C.Z) * (p.X - t.C.X) + (t.C.X - t.B.X) * (p.Z - t.C.Z)) / den;
                float w1 = ((t.C.Z - t.A.Z) * (p.X - t.C.X) + (t.A.X - t.C.X) * (p.Z - t.C.Z)) / den;
                if (w0 < 0 || w1 < 0 || w0 + w1 > 1) continue;
                float y = w0 * t.A.Y + w1 * t.B.Y + (1 - w0 - w1) * t.C.Y;
                if (y <= p.Y + 60) { if (y > best && y > p.Y - 1300) best = y; }
                else if (y < above) above = y;
            }
            if (best == float.MinValue) best = above;
            return best == float.MaxValue ? null : new Vector3(p.X, best + 10, p.Z);
        }
        spawns = spawns.Select(sp => (Floor(sp.Item1) ?? sp.Item1, sp.Item2)).ToList();
        Fill(pn.Startpos, (li, i) =>
        {
            var (p, yaw) = spawns[i % spawns.Count];
            float ring = i / spawns.Count * 6f, ang = i * 2.4f;  // (riders past the spawns placed: around them)
            var q = p + new Vector3(MathF.Cos(ang) * ring, 0, MathF.Sin(ang) * ring);
            return At(Floor(q) ?? p, yaw);
        });
        Fill(pn.GlobalDeadPos, (li, i) => At(spawns[0].Item1, spawns[0].Item2));
        Fill(pn.Enemypos, (li, i) => At(OnFloor(10)));
        // the events' points: each event's own range of the list - the points placed for it, else as many as the city's on
        // the floor (an event with none in the city and none placed: none)
        if (pn.Eventpos?.Array is { Length: > 0 } evLists && evParams != null)
        {
            var pts = new List<KAR_grPositionData>();
            var ps = evParams.Array;
            for (int e = 0; e < ps.Length; e++)
            {
                var mine = spec.EventPoints.Where(p => (int)p.Event == e).ToList();
                int n = mine.Count > 0 ? mine.Count : ps[e].PositionNum;
                ps[e].PositionStart = n > 0 ? pts.Count : 0;
                ps[e].PositionNum = n;
                if (mine.Count > 0) pts.AddRange(mine.Select(p => At(p.Position, p.Yaw)));
                else for (int i = 0; i < n; i++) pts.Add(At(OnFloor(10)));
                if (mine.Count > 0) log($"event {(EventKind)e}: {mine.Count} points placed");
            }
            evParams.Array = ps;
            evLists[0].PositionData = new HSDArrayAccessor<KAR_grPositionData> { Array = pts.ToArray() };
            evLists[0].Count = pts.Count;
            pn.Eventpos.Array = evLists;
        }
        else Fill(pn.Eventpos, (li, i) => At(OnFloor(10)));
        Fill(pn.Vehiclepos, (li, i) => At(OnFloor(10), rng.Next(360)));
        // boxes and power-ups: as many spots as the map asks for, over the floor, in the city's lists' proportions
        if (pn.ItemPos != null)
        {
            var arr = pn.ItemPos.Array;
            int had = arr.Sum(l => l.Count);
            for (int li = 0; li < arr.Length; li++)
            {
                if (arr[li].Count == 0) continue;
                int n = Math.Max(1, (int)Math.Round((double)arr[li].Count / had * spec.ItemSpots));
                arr[li].PositionData = new HSDArrayAccessor<KAR_grPositionData> { Array = Enumerable.Range(0, n).Select(_ => At(OnFloor(10))).ToArray() };
                arr[li].Count = n;
            }
            pn.ItemPos.Array = arr;
        }
        // Spawn areas (machines - Warp Stars and the rest - and items): each a box, two entries (corner A with its facing,
        // corner B) - the game puts a machine somewhere in it (801DE7C8), with no floor test: a small box on open, flat
        // ground, well apart from the others. (56 areas at most in a list: the machines' area weights, VcCommon.)
        var flat = floors.Where(t => t.Normal.Y >= 0.9f && t.Area > 400).ToList();
        if (flat.Count == 0) flat = floors;
        double flatTotal = flat.Sum(t => (double)t.Area);
        Vector3 OnFlat()
        {
            double r = rng.NextDouble() * flatTotal;
            foreach (var t in flat) { r -= t.Area; if (r <= 0) { float u = (float)rng.NextDouble(), v = (float)rng.NextDouble(); if (u + v > 1) { u = 1 - u; v = 1 - v; } return t.A + (t.B - t.A) * u + (t.C - t.A) * v; } }
            return flat[^1].A;
        }
        void Areas(HSDArrayAccessor<KAR_grAreaPositionList> lists, float height)
        {
            if (lists == null) return;
            var arr = lists.Array;
            foreach (var l in arr)
            {
                if (l.Count < 2) continue;
                int areas = Math.Min(l.Count / 2, 56);
                var centres = new List<Vector3>();
                for (int tries = 0; centres.Count < areas && tries < areas * 50; tries++)
                {
                    var p = OnFlat();
                    if (centres.All(q => Vector2.Distance(new Vector2(q.X, q.Z), new Vector2(p.X, p.Z)) > 200 * spec.Scale)) centres.Add(p);
                }
                while (centres.Count < areas) centres.Add(OnFlat());
                var data = new List<KAR_grAreaPositionData>();
                foreach (var p in centres)
                {
                    const float half = 50;
                    data.Add(new KAR_grAreaPositionData { X = p.X - half, Y = p.Y + 6, Z = p.Z - half, DX = 0, DY = 0, DZ = -1 });
                    data.Add(new KAR_grAreaPositionData { X = p.X + half, Y = p.Y + 6 + height, Z = p.Z + half, DX = 0, DY = 0, DZ = -1 });
                }
                l.AreaPosition = new HSDArrayAccessor<KAR_grAreaPositionData> { Array = data.ToArray() };
                l.Count = data.Count;
            }
            lists.Array = arr;
        }
        Areas(pn.ItemAreaPos, 150);
        Areas(pn.VehicleAreapos, 0);
        log($"positions: {spawns.Count} spawns, {pn.ItemPos?.Array.Sum(l => l.Count) ?? 0} item spots");
    }
}
