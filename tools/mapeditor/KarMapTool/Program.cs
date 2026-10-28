// KarMapTool: the City Trial map toolchain's command line (docs/MAPS.md).
//   KarMapTool dump <Gr*.dat>   what a stage file holds: its roots, the stage parameters, collision, zones, positions
using HSDRaw;
using HSDRaw.AirRide.Gr;
using HSDRaw.AirRide.Gr.Data;
using HSDRaw.Common;
using HSDRaw.GX;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "dump") return Dump(args[1]);
        if (args.Length >= 3 && args[0] == "city")  // city <disc dir> <out dir>: the original city as a map (its model as glTF, its spawns and events' points), for the editor
        {
            string disc = args[1], outDir = args[2];
            Directory.CreateDirectory(outDir);
            var gm = new HSDRawFile(Path.Combine(disc, "GrCity1Model.dat")).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var model = Path.Combine(outDir, "city.gltf");
            // (the textures as the game lays them: repeated and moved as each one's settings say; placed in the world)
            var es = new HSDRawViewer.Converters.ModelExportSettings { Directory = Path.GetFullPath(outDir) + "\\", ExportScaledUVs = true, ExportTransformedUVs = true };
            var exp = new HSDRawViewer.Converters.ModelExporter(gm.MainModel.RootNode, es, new HSDRawViewer.Tools.Animation.JointMap());
            var scene = exp.Scene;
            var matColour = new Dictionary<string, System.Numerics.Vector4>();
            foreach (var (m, mobj) in exp.MaterialToMObj)
            {
                // a material whose texture isn't marked as its colour: its first one
                if (m.DiffuseMap == null && mobj?.Textures?.List.FirstOrDefault(t => t.ImageData?.ImageData != null) is { } t0)
                {
                    var name = $"tex_{HSDRawViewer.Extensions.ImporterExtensions.ComputeHash(t0.ImageData.ImageData):X8}.png";
                    if (!File.Exists(Path.Combine(outDir, name))) HSDRawViewer.TOBJExtentions.SaveImagePNG(t0, Path.Combine(outDir, name));
                    m.DiffuseMap = new IONET.Core.Model.IOTexture { Name = name, FilePath = name, WrapS = t0.WrapS == HSDRaw.GX.GXWrapMode.CLAMP ? IONET.Core.Model.WrapMode.CLAMP : t0.WrapS == HSDRaw.GX.GXWrapMode.MIRROR ? IONET.Core.Model.WrapMode.MIRROR : IONET.Core.Model.WrapMode.REPEAT, WrapT = t0.WrapT == HSDRaw.GX.GXWrapMode.CLAMP ? IONET.Core.Model.WrapMode.CLAMP : t0.WrapT == HSDRaw.GX.GXWrapMode.MIRROR ? IONET.Core.Model.WrapMode.MIRROR : IONET.Core.Model.WrapMode.REPEAT };
                }
                var c = mobj?.Material?.DiffuseColor ?? System.Drawing.Color.White;
                matColour[m.Name] = new System.Numerics.Vector4(c.R / 255f, c.G / 255f, c.B / 255f, c.A / 255f);
            }
            var uvSet = scene.Materials.ToDictionary(m => m.Name, m => m.DiffuseMap?.UVChannel ?? 0);
            var done = new HashSet<IONET.Core.Model.IOVertex>();
            foreach (var mesh in scene.Models[0].Meshes)
                foreach (var poly in mesh.Polygons)
                {
                    int set = poly.MaterialName != null && uvSet.TryGetValue(poly.MaterialName, out int s) ? s : 0;
                    var mc = poly.MaterialName != null && matColour.TryGetValue(poly.MaterialName, out var col) ? col : System.Numerics.Vector4.One;
                    foreach (int i in poly.Indicies)
                    {
                        var v = mesh.Vertices[i];
                        if (!done.Add(v)) continue;
                        // the texture's own UV set first, in glTF's way up (the writer turns V over again)
                        if (set > 0 && set < v.UVs.Count) (v.UVs[0], v.UVs[set]) = (v.UVs[set], v.UVs[0]);
                        if (v.UVs.Count > 0) v.UVs[0] = new System.Numerics.Vector2(v.UVs[0].X, 1 - v.UVs[0].Y);
                        if (v.Colors.Count == 0) v.Colors.Add(mc);
                    }
                }
            GltfIO.Write(scene, model);
            var gd = (KAR_grData)new HSDRawFile(Path.Combine(disc, "GrCity1.dat")).Roots[0].Data;
            float Yaw(KAR_grPositionData p) => MathF.Atan2(-p.FX, -p.FZ) * 180f / MathF.PI;
            var spec = new MapSpec { Name = "City Trial (original)", Model = model, Center = false };
            var sp = gd.PositionNode.Startpos?.Array;
            if (sp is { Length: > 0 } && sp[0].PositionData?.Array is { } spd)
                foreach (var p in spd) spec.Spawns.Add(new Placed { X = p.X, Y = p.Y, Z = p.Z, Yaw = Yaw(p) });
            var evFile = Path.Combine(disc, "GrCity1Event.dat");
            var evPts = gd.PositionNode.Eventpos?.Array is { Length: > 0 } el ? el[0].PositionData?.Array : null;
            if (File.Exists(evFile) && evPts != null)
            {
                var evRoot = new HSDRawFile(evFile).Roots.First(r => r.Name == "grEventDataAllCity1");
                var ps = new KAR_grEventDataAllCity1 { _s = evRoot.Data._s }.EventNode.EventParams.Array;
                foreach (EventKind k in Enum.GetValues<EventKind>())
                {
                    if ((int)k >= ps.Length) continue;
                    for (int i = ps[(int)k].PositionStart; i < ps[(int)k].PositionStart + ps[(int)k].PositionNum && i < evPts.Length; i++)
                        spec.EventPoints.Add(new EventPoint { X = evPts[i].X, Y = evPts[i].Y, Z = evPts[i].Z, Yaw = Yaw(evPts[i]), Event = k });
                }
            }
            spec.Model = "city.gltf";
            File.WriteAllText(Path.Combine(outDir, "map.json"), System.Text.Json.JsonSerializer.Serialize(spec, new System.Text.Json.JsonSerializerOptions { WriteIndented = true, DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull }));
            Console.WriteLine($"{model}: {spec.Spawns.Count} spawns, {spec.EventPoints.Count} event points");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "xlu")  // (the see-through meshes: their materials and vertex attributes)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            int n = 0;
            foreach (var j in gm.MainModel.RootNode.TreeList)
                for (var d = j.Dobj; d != null; d = d.Next)
                {
                    if (d.Mobj == null || d.Pobj == null || (d.Mobj.RenderFlags & RENDER_MODE.XLU) == 0) continue;
                    var v0 = d.Pobj.ToDisplayList().Vertices; if (v0.Count == 0 || v0.Average(v => v.CLR0.G - v.CLR0.R) < 0.1f || n++ > 6) continue;
                    var attrs = d.Pobj == null ? "" : string.Join(",", d.Pobj.ToGXAttributes().Select(a => a.AttributeName));
                    var t = d.Mobj.Textures;
                    var clr = d.Pobj?.ToDisplayList().Vertices.Take(3).Select(v => $"({v.CLR0.R:F2},{v.CLR0.G:F2},{v.CLR0.B:F2},{v.CLR0.A:F2})");
                    Console.WriteLine($"{d.Mobj.RenderFlags} mat {d.Mobj.Material?.DiffuseColor} tex {t?.Flags} {t?.ImageData?.Format} pe {d.Mobj.PEDesc != null} attrs {attrs} clr {string.Join(" ", clr ?? Array.Empty<string>())} joint {j.Flags}");
                }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "regions")  // (the main model's culling: each view region's box and meshes)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var b = gm.MainModel.ModelBounding;
            int k = 0;
            foreach (var r in b.ViewRegions ?? Array.Empty<KAR_grViewRegion>())
                if (k++ < 3) Console.WriteLine($"region: x {r.MinX:F0}..{r.MaxX:F0} z {r.MinZ:F0}..{r.MaxZ:F0}: {r.Indices.Length} meshes, {string.Join(",", r.Indices.Take(6))}..{string.Join(",", r.Indices.TakeLast(4))}");
            Console.WriteLine($"always: {b.Indices.Length}; boxes {b.StaticBoundingBoxes.Length}; joints {gm.MainModel.JobjCount} meshes {gm.MainModel.DobjCount}");
            int n = 0;
            foreach (var j in gm.MainModel.RootNode.TreeList) { for (var d = j.Dobj; d != null; d = d.Next) n++; }
            Console.WriteLine($"tree meshes {n}; root's children: {gm.MainModel.RootNode.Children.Length}");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "vtxflags")  // (the model's meshes: render flags, by whether their vertices have colours)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var counts = new Dictionary<string, int>();
            foreach (var j in gm.MainModel.RootNode.TreeList)
                for (var d = j.Dobj; d != null; d = d.Next)
                {
                    if (d.Pobj == null || d.Mobj == null) continue;
                    bool clr = d.Pobj.ToGXAttributes().Any(a => a.AttributeName == GXAttribName.GX_VA_CLR0);
                    var k = $"clr0={clr} {d.Mobj.RenderFlags} tev={(d.Mobj.Textures?.TEV != null)} pe={(d.Mobj.PEDesc != null)} mat={d.Mobj.Material?.DiffuseColor.Name} amb={d.Mobj.Material?.AmbientColor.Name}";
                    counts[k] = counts.GetValueOrDefault(k) + 1;
                }
            foreach (var (k, n) in counts.OrderByDescending(x => x.Value).Take(15)) Console.WriteLine($"{n,5} {k}");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "fog")  // (the stage's fog: its kind, start, end and colour)
        {
            var gd = (KAR_grData)new HSDRawFile(args[1]).Roots[0].Data;
            var fn = gd.FogNode;
            Console.WriteLine($"fog node: {fn?._s.Length} bytes");
            if (fn?.FogData?.Fog is HSD_FogDesc f) Console.WriteLine($"fog: type {f.Type} start {f.Start} end {f.End} colour {f.Color}");
            if (fn?.FogTypes?.FogData?.Array is { } types)
                for (int i = 0; i < types.Length; i++)
                {
                    var s = types[i]._s;
                    Console.Write($"  type {i}:");
                    for (int o = 0; o < s.Length; o += 4) Console.Write(o is 0 or 4 or 0x10 or 0x14 or 0x20 or 0x24 ? $" {o:X2}:{s.GetInt32(o):X8}" : $" {o:X2}:{s.GetFloat(o):G5}");
                    Console.WriteLine();
                }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "events")  // (City Trial's events: each one's points in the stage's event list)
        {
            var ev = new HSDRawFile(args[1]).Roots[0].Data;
            var all = new KAR_grEventDataAllCity1 { _s = ev._s };
            var ps = all.EventNode.EventParams.Array;
            for (int i = 0; i < ps.Length; i++)
                Console.WriteLine($"event {i}: bgm {ps[i].BgmIndex} sky {ps[i].SkyIndex} points {ps[i].PositionStart}+{ps[i].PositionNum} param {(ps[i].Param == null ? 0 : ps[i].Param._s.Length)} bytes");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "positions")  // (a stage's position lists: each list's joints and points)
        {
            var gd = (HSDRaw.AirRide.Gr.Data.KAR_grData)new HSDRawFile(args[1]).Roots[0].Data;
            var pn = gd.PositionNode;
            var names = new List<string>();
            if (pn.PositionJoint != null) foreach (var j in pn.PositionJoint.TreeList) names.Add(j.ClassName ?? "");
            void Show(string what, HSDArrayAccessor<HSDRaw.AirRide.Gr.Data.KAR_grPositionList> lists)
            {
                if (lists == null) { Console.WriteLine($"{what}: none"); return; }
                var arr = lists.Array;
                Console.WriteLine($"{what}: {arr.Length} lists");
                for (int i = 0; i < arr.Length; i++)
                {
                    var ji = arr[i].JointIndices?.Array ?? Array.Empty<int>();
                    var pd = arr[i].PositionData?.Array;
                    var p0 = pd != null && pd.Length > 0 ? $"({pd[0].X:F0},{pd[0].Y:F0},{pd[0].Z:F0})" : "";
                    Console.WriteLine($"  [{i}] {arr[i].Count} points, joints {string.Join(",", ji.Take(8))}{(ji.Length > 8 ? "..." : "")} {p0}");
                }
            }
            Show("start", pn.Startpos); Show("enemy", pn.Enemypos); Show("gravity", pn.Gravitypos); Show("airflow", pn.Airflowpos);
            Show("conveyor", pn.Conveyorpos); Show("item", pn.ItemPos); Show("event", pn.Eventpos); Show("vehicle", pn.Vehiclepos);
            Show("globaldead", pn.GlobalDeadPos); Show("localdead", pn.LocalDeadPos); Show("yakumono", pn.Yakumonopos);
            Console.WriteLine($"joints: {names.Count}");
            return 0;
        }
        if (args.Length >= 3 && args[0] == "sky")  // (the stage model's sky: its joints, meshes, textures - saved as PNG - and size)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            Console.Write("grModel: ");
            for (int o = 0; o < gm._s.Length; o += 4) Console.Write($"{o:X2}:{gm._s.GetInt32(o):X8}{(gm._s.GetReference<HSDAccessor>(o) != null ? "*" : "")} ");
            Console.WriteLine();
            var sky = gm.SkyboxModel;
            Directory.CreateDirectory(args[2]);
            int n = 0;
            foreach (var j in sky.JOBJRoot.TreeList)
            {
                Console.WriteLine($"joint flags {j.Flags} T ({j.TX:F0},{j.TY:F0},{j.TZ:F0}) S ({j.SX:F2},{j.SY:F2},{j.SZ:F2})");
                for (var d = j.Dobj; d != null; d = d.Next)
                {
                    var m = d.Mobj;
                    var lo = new System.Numerics.Vector3(float.MaxValue); var hi = new System.Numerics.Vector3(float.MinValue);
                    for (var p = d.Pobj; p != null; p = p.Next)
                        foreach (var dl in p.ToDisplayList().Vertices)
                        { var v = new System.Numerics.Vector3(dl.POS.X, dl.POS.Y, dl.POS.Z); lo = System.Numerics.Vector3.Min(lo, v); hi = System.Numerics.Vector3.Max(hi, v); }
                    Console.WriteLine($"  mesh {n}: render {m?.RenderFlags} bounds {lo}..{hi}");
                    if (m?.PEDesc is { } pe) Console.WriteLine($"    pe: flags {pe.Flags} blend {pe.BlendMode} {pe.SrcFactor}/{pe.DstFactor} depth {pe.DepthFunction} alpha {pe.AlphaComp0} {pe.AlphaRef0}");
                    var vs = d.Pobj?.ToDisplayList().Vertices;
                    if (vs != null) foreach (var v in vs.Where((_, i) => i % Math.Max(1, vs.Count / 6) == 0).Take(7)) Console.WriteLine($"      ({v.POS.X:F0},{v.POS.Y:F0},{v.POS.Z:F0}) uv ({v.TEX0.X:F3},{v.TEX0.Y:F3})");
                    for (var t = m?.Textures; t != null; t = t.Next)
                    {
                        Console.WriteLine($"    texture {t.ImageData?.Width}x{t.ImageData?.Height} {t.ImageData?.Format} wrap {t.WrapS}/{t.WrapT} flags {t.Flags} scale {t.RepeatS}x{t.RepeatT}");
                        var px = t.GetDecodedImageData();
                        using var img = SixLabors.ImageSharp.Image.LoadPixelData<SixLabors.ImageSharp.PixelFormats.Bgra32>(px, t.ImageData.Width, t.ImageData.Height);
                        SixLabors.ImageSharp.ImageExtensions.SaveAsPng(img, Path.Combine(args[2], $"sky_{n}.png"));
                    }
                    n++;
                }
            }
            Console.WriteLine($"motion: anim {sky.MotionJoint?.AnimJoint != null}, material anim {sky.MotionJoint?.MatAnimJoint != null}");
            return 0;
        }
        // soh <oot.o2r> <scene, e.g. spot00> <out dir> [scale]: a scene of Ship of Harkinian's as a map (SohScene.cs)
        if (args.Length >= 4 && args[0] == "soh")
        {
            float sc = args.Length >= 5 && !args[4].StartsWith("--") ? float.Parse(args[4], System.Globalization.CultureInfo.InvariantCulture) : 0.25f;
            // (with the places its exits lead to, as areas joined by warps; --alone: the scene by itself)
            if (args.Contains("--alone")) SohScene.Import(args[1], args[2], args[3], sc, Console.WriteLine);
            else SohWorld.Import(args[1], args[2], args[3], sc, Console.WriteLine);
            return 0;
        }
        if (args.Length >= 3 && args[0] == "roundtrip")
        {
            new HSDRawFile(args[1]).Save(args[2]);
            Console.WriteLine($"{args[1]} -> {args[2]} ({new FileInfo(args[1]).Length} -> {new FileInfo(args[2]).Length} bytes)");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "tree")  // (the collision partition's header, raw)
        {
            var gd = (KAR_grData)new HSDRawFile(args[1]).Roots[0].Data;
            var t = gd.PartitionNode.Partition;
            for (int o = 0; o < 0x5C; o += 4) Console.Write($"{o:X2}:{t._s.GetInt32(o):X8} ");
            Console.WriteLine();
            var bk = t.Buckets;
            Console.WriteLine($"{bk.Length} buckets; first bucket raw:");
            for (int o = 0; o < 0x50; o += 2) Console.Write($"{o:X2}:{(ushort)bk[0]._s.GetInt16(o):X4} ");
            Console.WriteLine();
            return 0;
        }
        if (args.Length >= 2 && args[0] == "bounding")  // (the main model's culling data, raw)
        {
            var b = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First().MainModel.ModelBounding;
            Console.Write("header: ");
            for (int o = 0; o < 0x20; o += 4) Console.Write($"{o:X2}:{b._s.GetInt32(o):X8} ");
            Console.WriteLine();
            var regions = b._s.GetReference<HSDAccessor>(0x00);
            if (regions != null)
                for (int r = 0; r < 3; r++)
                {
                    Console.Write($"region {r}: ");
                    for (int o = 0; o < 0x20; o += 4) Console.Write($"{regions._s.GetInt32(r * 0x20 + o):X8} ");
                    var idx = regions._s.GetReference<HSDAccessor>(r * 0x20);
                    if (idx != null) Console.Write($" -> {idx._s.Length} bytes: " + string.Join(",", Enumerable.Range(0, Math.Min(20, idx._s.Length / 2)).Select(i => (ushort)idx._s.GetInt16(i * 2))));
                    Console.WriteLine();
                }
            var ind = b._s.GetReference<HSDAccessor>(0x18);
            if (ind != null) Console.WriteLine($"indices: {ind._s.Length} bytes: " + string.Join(",", Enumerable.Range(0, Math.Min(40, ind._s.Length / 2)).Select(i => (ushort)ind._s.GetInt16(i * 2))));
            return 0;
        }
        if (args.Length >= 2 && args[0] == "meshes")  // (the main model's first meshes: material, texture, polygon flags)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            int n = 0;
            foreach (var j in gm.MainModel.RootNode.TreeList)
                for (var d = j.Dobj; d != null && n < 8; d = d.Next, n++)
                {
                    var m = d.Mobj;
                    var t = m?.Textures;
                    Console.WriteLine($"mesh {n} (joint flags {j.Flags}): render {m?.RenderFlags}; diffuse {m?.Material?.DiffuseColor}; ambient {m?.Material?.AmbientColor}; " +
                                      $"texture {(t == null ? "none" : $"{t.ImageData?.Width}x{t.ImageData?.Height} {t.ImageData?.Format} flags {t.Flags}")}; pobj {(d.Pobj == null ? "none" : d.Pobj.Flags.ToString())}");
                }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "pobjstats")  // (the main model's meshes: the biggest, by vertices and display list size)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var rows = new List<(int i, int verts, int dl, int prims, string tex)>();
            int n = 0;
            foreach (var j in gm.MainModel.RootNode.TreeList)
                for (var d = j.Dobj; d != null; d = d.Next, n++)
                    for (var p = d.Pobj; p != null; p = p.Next)
                    {
                        var dl = p.ToDisplayList();
                        var t = d.Mobj?.Textures;
                        rows.Add((n, dl.Vertices.Count, p.DisplayListBuffer?.Length ?? 0, dl.Primitives.Count, t == null ? "-" : $"{t.ImageData?.Width}x{t.ImageData?.Height} {t.ImageData?.Format}"));
                    }
            foreach (var r in rows.OrderByDescending(r => r.dl).Take(12)) Console.WriteLine($"mesh {r.i}: {r.verts} vertices, display list {r.dl} bytes, {r.prims} primitives, texture {r.tex}");
            Console.WriteLine($"{rows.Count} polygon groups; largest texture: " + string.Join(", ", rows.Select(r => r.tex).Distinct().OrderByDescending(s => s.Length).Take(3)));
            return 0;
        }
        if (args.Length >= 2 && args[0] == "matanim")  // (the stage's model motions: which joints' meshes have material animations)
        {
            var f = new HSDRawFile(args[1]);
            var gm = f.Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var root = gm.MainModel.RootNode;
            int rd = 0; for (var d = root.Dobj; d != null; d = d.Next) rd++;
            Console.WriteLine($"root joint: {rd} meshes, {root.TreeList.Count} joints");
            var motion = f.Roots.FirstOrDefault(r => r.Name.StartsWith("grModelMotion"));
            if (motion != null)
            {
                var arr = new HSDArrayAccessor<KAR_grModelMotion> { _s = motion.Data._s };
                int i = 0;
                foreach (var m in arr.Array)
                {
                    var mj = m.MatAnimJoint;
                    int withAnim = 0, rootAnims = 0;
                    if (mj != null)
                    {
                        for (var a = mj.MaterialAnimation; a != null; a = a.Next) rootAnims++;
                        foreach (var j in mj.TreeList) for (var a = j.MaterialAnimation; a != null; a = a.Next) if (a.TextureAnimation != null || a.AnimationObject != null) withAnim++;
                    }
                    Console.WriteLine($"motion {i++}: joint anim {m.AnimJoint != null}, material anim {mj != null} (root's material anims {rootAnims}, with tracks {withAnim})");
                }
            }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "motion")  // (the stage model's +0x0C: what it points to, raw, and any animation in it)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var m = gm._s.GetReference<HSDAccessor>(0x0C);
            void Raw(HSDAccessor x, string tag, int depth)
            {
                if (x == null || depth > 3) return;
                Console.Write($"{new string(' ', depth * 2)}{tag} ({x._s.Length} bytes): ");
                for (int o = 0; o + 4 <= Math.Min(x._s.Length, 0x40); o += 4) Console.Write($"{o:X2}:{x._s.GetInt32(o):X8}{(x._s.GetReference<HSDAccessor>(o) != null ? "*" : "")} ");
                Console.WriteLine();
                for (int o = 0; o + 4 <= Math.Min(x._s.Length, 0x40); o += 4) Raw(x._s.GetReference<HSDAccessor>(o), $"+{o:X2}", depth + 1);
            }
            Raw(m, "grModel+0C", 0);
            return 0;
        }
        if (args.Length >= 2 && args[0] == "root")  // (the main model's root joint and its first children: their transforms)
        {
            var gm = new HSDRawFile(args[1]).Roots.Select(r => r.Data).OfType<KAR_grModel>().First();
            var j = gm.MainModel.RootNode;
            void Show(HSD_JOBJ x, string tag) => Console.WriteLine($"{tag}: T ({x.TX}, {x.TY}, {x.TZ}) R ({x.RX}, {x.RY}, {x.RZ}) S ({x.SX}, {x.SY}, {x.SZ}) flags {x.Flags}");
            Show(j, "root");
            int n = 0;
            for (var c = j.Child; c != null && n < 6; c = c.Next, n++) Show(c, $"child {n}");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "pos")  // (the first start position, every field)
        {
            var gd = (KAR_grData)new HSDRawFile(args[1]).Roots[0].Data;
            foreach (var (name, lists) in new[] { ("start", gd.PositionNode.Startpos), ("item", gd.PositionNode.ItemPos), ("vehicle", gd.PositionNode.Vehiclepos), ("event", gd.PositionNode.Eventpos) })
            {
                var d = lists.Array[0].PositionData.Array[0];
                Console.WriteLine($"{name}: size {d._s.Length}: " + string.Join(" ", Enumerable.Range(0, d._s.Length / 4).Select(i => d._s.GetFloat(i * 4).ToString("F3"))));
            }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "winding")  // (which way the floor triangles' (V1, V2, V3) normal points)
        {
            var gd = (KAR_grData)new HSDRawFile(args[1]).Roots[0].Data;
            var v = gd.CollisionNode.Vertices;
            int up = 0, down = 0;
            foreach (var t in gd.CollisionNode.Triangles.Where(t => t.Flags == KCCollFlag.Floor))
            {
                var a = new System.Numerics.Vector3(v[t.V1].X, v[t.V1].Y, v[t.V1].Z);
                var b = new System.Numerics.Vector3(v[t.V2].X, v[t.V2].Y, v[t.V2].Z);
                var c = new System.Numerics.Vector3(v[t.V3].X, v[t.V3].Y, v[t.V3].Z);
                if (System.Numerics.Vector3.Cross(b - a, c - a).Y > 0) up++; else down++;
            }
            Console.WriteLine($"floor triangles: (V1,V2,V3) normal up {up}, down {down}");
            var tr = gd.PartitionNode.Partition;
            Console.WriteLine($"tree: {tr.BucketsCount} buckets, collidable {tr.CollidableTriangleCount}; bucket 0 depth {tr.Buckets[0].Depth} children {tr.Buckets[0].Child1},{tr.Buckets[0].Child2}");
            return 0;
        }
        if (args.Length >= 4 && args[0] == "build")
        {
            MapBuilder.Build(MapSpec.Load(args[1]), args[2], args[3], Console.WriteLine);
            return 0;
        }
        Console.Error.WriteLine("usage: KarMapTool dump <Gr*.dat> | roundtrip <in.dat> <out.dat> | build <map.json> <city files dir> <out dir>");
        return 2;
    }

    static int Dump(string path)
    {
        var file = new HSDRawFile(path);
        Console.WriteLine($"{Path.GetFileName(path)}: {file.Roots.Count} roots");
        foreach (var r in file.Roots) Console.WriteLine($"  root {r.Name}: {r.Data?.GetType().Name} ({r.Data?._s.Length} bytes)");
        foreach (var r in file.Roots)
        {
            if (r.Data is KAR_grData gd) DumpGrData(gd);
            else if (r.Data is KAR_grModel gm)
            {
                var mm = gm.MainModel;
                Console.WriteLine($"  main model: {CountJoints(mm?.RootNode)} joints ({mm?.JobjCount}), {CountDObjs(mm?.RootNode)} meshes ({mm?.DobjCount}), pobjs {mm?.PobjCount}");
                var b = mm?.ModelBounding;
                if (b != null)
                {
                    Console.WriteLine($"  bounding: {b.ViewRegionCount} view regions, {b.StaticBoxNum} static boxes, {b.DynamicBoxNum} dynamic boxes, {b.Count} indices (max {(b.Indices?.Length > 0 ? b.Indices.Max() : 0)})");
                    foreach (var v in (b.ViewRegions ?? Array.Empty<KAR_grViewRegion>()).Take(4))
                        Console.WriteLine($"    view region ({v.MinX:F0},{v.MinY:F0},{v.MinZ:F0})..({v.MaxX:F0},{v.MaxY:F0},{v.MaxZ:F0}): {v.Count} indices, first {string.Join(",", (v.Indices ?? Array.Empty<ushort>()).Take(10))}");
                    foreach (var s in (b.DynamicBoundingBoxes ?? Array.Empty<KAR_grDynamicBoundingBoxes>()).Take(3))
                        Console.WriteLine($"    dynamic box bone {s.BoneIndex}: {s.Count} indices, first {string.Join(",", (s.Indices ?? Array.Empty<ushort>()).Take(10))}");
                    Console.WriteLine($"    indices first {string.Join(",", (b.Indices ?? Array.Empty<ushort>()).Take(20))}");
                }
                Console.WriteLine($"  skybox: {(gm.SkyboxModel != null ? "yes" : "no")}");
            }
            else if (r.Name.StartsWith("grModelMotion")) Console.WriteLine($"  {r.Name}: {r.Data._s.Length / 4} entries");
            else if (r.Data is HSD_JOBJ j) Console.WriteLine($"  {r.Name}: {CountJoints(j)} joints, {CountDObjs(j)} meshes");
        }
        return 0;
    }

    static int CountJoints(HSD_JOBJ j) => j == null ? 0 : 1 + CountJoints(j.Child) + CountJoints(j.Next);
    static int CountDObjs(HSD_JOBJ j)
    {
        if (j == null) return 0;
        int n = 0;
        for (var d = j.Dobj; d != null; d = d.Next) n++;
        return n + CountDObjs(j.Child) + CountDObjs(j.Next);
    }

    static void DumpGrData(KAR_grData gd)
    {
        var st = gd.StageNode;
        if (st != null)
            Console.WriteLine($"  stage: scale {st.StageScale}, gravity {st.GravityStrength} ({st.GravityDirectionX}, {st.GravityDirectionY}, {st.GravityDirectionZ}), " +
                              $"minimap scale {st.MinimapScale}, out of bounds ({st.OoBMinXArea}, {st.OoBMinYArea}, {st.OoBMinZArea})..({st.OoBMaxXArea}, {st.OoBMaxYArea}, {st.OoBMaxZArea})");
        var c = gd.CollisionNode;
        if (c != null)
        {
            Console.WriteLine($"  collision: {c.VertexCount} vertices, {c.TriangleCount} triangles, {c.JointCount} joints; zones: {c.ZoneVertexCount} vertices, {c.ZoneTriangleCount} triangles, {c.ZoneJointCount} joints");
            var tris = c.Triangles ?? Array.Empty<KAR_CollisionTriangle>();
            foreach (var g in tris.GroupBy(t => t.Flags).OrderBy(g => g.Key)) Console.WriteLine($"    triangles {g.Key}: {g.Count()}");
            var zt = c.ZoneTriangles ?? Array.Empty<KAR_ZoneCollisionTriangle>();
            foreach (var g in zt.GroupBy(t => t.Kind).OrderBy(g => g.Key)) Console.WriteLine($"    zone kind {g.Key}: {g.Count()} triangles");
            var joints = c.Joints ?? Array.Empty<KAR_CollisionJoint>();
            foreach (var g in joints.GroupBy(j => j.Kind).OrderBy(g => g.Key)) Console.WriteLine($"    collision joints of kind {g.Key}: {g.Count()}");
        }
        var p = gd.PositionNode;
        if (p != null)
        {
            void List(string name, HSDArrayAccessor<KAR_grPositionList> lists)
            {
                if (lists == null) { Console.WriteLine($"  positions {name}: none"); return; }
                var a = lists.Array;
                var first = a.Select(l => l.PositionData?.Array).FirstOrDefault(d => d != null && d.Length > 0);
                Console.WriteLine($"  positions {name}: {a.Length} lists, {a.Sum(l => l.Count)} positions" +
                                  (first != null ? $" (first {first[0].X:F1} {first[0].Y:F1} {first[0].Z:F1})" : ""));
            }
            List("start", p.Startpos); List("enemy", p.Enemypos); List("gravity", p.Gravitypos); List("airflow", p.Airflowpos);
            List("conveyor", p.Conveyorpos); List("item", p.ItemPos); List("event", p.Eventpos); List("vehicle", p.Vehiclepos);
            List("global dead", p.GlobalDeadPos); List("local dead", p.LocalDeadPos); List("yakumono", p.Yakumonopos);
            Console.WriteLine($"  item areas: {p.ItemAreaPos?.Array.Length ?? 0}, vehicle areas: {p.VehicleAreapos?.Array.Length ?? 0}");
            void Detail(string name, HSDArrayAccessor<KAR_grPositionList> lists)
            {
                if (lists == null) return;
                int i = 0;
                foreach (var l in lists.Array)
                {
                    var d = l.PositionData?.Array;
                    var ji = l.JointIndices?.Array;
                    Console.WriteLine($"    {name}[{i++}]: count {l.Count}, data {d?.Length ?? 0}, joint indices {(ji == null ? "none" : string.Join(",", ji.Take(8)))}" +
                                      (d != null && d.Length > 0 ? $"; first ({d[0].X:F0}, {d[0].Y:F0}, {d[0].Z:F0}) fwd ({d[0].FX:F2}, {d[0].FY:F2}, {d[0].FZ:F2})" : ""));
                }
            }
            Detail("start", p.Startpos); Detail("item", p.ItemPos); Detail("vehicle", p.Vehiclepos); Detail("global dead", p.GlobalDeadPos);
            Detail("local dead", p.LocalDeadPos); Detail("event", p.Eventpos); Detail("yakumono", p.Yakumonopos);
            void Areas(string name, HSDArrayAccessor<KAR_grAreaPositionList> lists)
            {
                if (lists == null) return;
                int i = 0;
                foreach (var l in lists.Array)
                {
                    var d = l.AreaPosition?.Array;
                    Console.WriteLine($"    {name}[{i++}]: count {l.Count}, data {d?.Length ?? 0}" +
                                      (d != null && d.Length > 0 ? $"; first ({d[0].X:F0}, {d[0].Y:F0}, {d[0].Z:F0}) extent ({d[0].DX:F0}, {d[0].DY:F0}, {d[0].DZ:F0})" : ""));
                }
            }
            Areas("item area", p.ItemAreaPos); Areas("vehicle area", p.VehicleAreapos);
            Console.WriteLine($"  position joints: {CountJoints(p.PositionJoint)}");
        }
        Console.WriteLine($"  yakumono: {gd.YakumonoNode?.YakumonoCount ?? 0}; respawns: {gd.RespawnNode?.Count ?? 0}; " +
                          $"partition buckets: {gd.PartitionNode?.Partition?.BucketsCount ?? 0}; events: {gd.EventNode?.Array.Length ?? 0}; " +
                          $"splines: {(gd.SplineNode != null ? "yes" : "no")}; rails: {(gd.RailCollNode != null ? "yes" : "no")}; fog: {(gd.FogNode != null ? "yes" : "no")}");
    }
}

