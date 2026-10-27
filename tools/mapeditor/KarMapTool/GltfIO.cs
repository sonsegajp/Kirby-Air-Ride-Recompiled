// glTF 2.0 (.gltf with its .bin and images beside it, or .glb) to and from IONET's scenes: what Blender reads and writes
// best, so a map goes model -> Blender (edited, looked at) -> the builder through it. Meshes with positions, normals, one
// set of texture coordinates and vertex colours (COLOR_0), triangles; materials with a base colour texture and its wrap
// modes. Node transforms are applied (the scene as placed).
using IONET.Core;
using IONET.Core.Model;
using System.Numerics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

public static class GltfIO
{
    const int FLOAT = 5126, UBYTE = 5121, USHORT = 5123, UINT = 5125;
    const int REPEAT = 10497, CLAMP = 33071, MIRROR = 33648;

    // ---- writing ----
    public static void Write(IOScene scene, string path)
    {
        var bin = new MemoryStream();
        var bufferViews = new JsonArray();
        var accessors = new JsonArray();
        int View(byte[] data, int target)
        {
            while (bin.Length % 4 != 0) bin.WriteByte(0);
            var v = new JsonObject { ["buffer"] = 0, ["byteOffset"] = bin.Length, ["byteLength"] = data.Length };
            if (target != 0) v["target"] = target;
            bin.Write(data);
            bufferViews.Add(v);
            return bufferViews.Count - 1;
        }
        int Floats(IList<float> f, int comps, string type, bool bounds)
        {
            var b = new byte[f.Count * 4];
            Buffer.BlockCopy(f.ToArray(), 0, b, 0, b.Length);
            var acc = new JsonObject { ["bufferView"] = View(b, 34962), ["componentType"] = FLOAT, ["count"] = f.Count / comps, ["type"] = type };
            if (bounds)
            {
                var mn = new JsonArray(); var mx = new JsonArray();
                for (int c = 0; c < comps; c++)
                {
                    float lo = float.MaxValue, hi = float.MinValue;
                    for (int i = c; i < f.Count; i += comps) { lo = Math.Min(lo, f[i]); hi = Math.Max(hi, f[i]); }
                    mn.Add(lo); mx.Add(hi);
                }
                acc["min"] = mn; acc["max"] = mx;
            }
            accessors.Add(acc);
            return accessors.Count - 1;
        }
        int Indices(List<uint> idx)
        {
            var b = new byte[idx.Count * 4];
            Buffer.BlockCopy(idx.ToArray(), 0, b, 0, b.Length);
            accessors.Add(new JsonObject { ["bufferView"] = View(b, 34963), ["componentType"] = UINT, ["count"] = idx.Count, ["type"] = "SCALAR" });
            return accessors.Count - 1;
        }

        var images = new JsonArray();
        var textures = new JsonArray();
        var samplers = new JsonArray();
        var materials = new JsonArray();
        var matIndex = new Dictionary<string, int>();
        static int Wrap(WrapMode w) => w == WrapMode.CLAMP ? CLAMP : w == WrapMode.MIRROR ? MIRROR : REPEAT;
        foreach (var m in scene.Materials)
        {
            var mat = new JsonObject { ["name"] = m.Name, ["doubleSided"] = false };
            var pbr = new JsonObject { ["metallicFactor"] = 0.0, ["roughnessFactor"] = 1.0 };
            if (m.DiffuseMap != null && !string.IsNullOrEmpty(m.DiffuseMap.FilePath))
            {
                images.Add(new JsonObject { ["uri"] = m.DiffuseMap.FilePath.Replace('\\', '/') });
                samplers.Add(new JsonObject { ["magFilter"] = 9729, ["minFilter"] = 9987, ["wrapS"] = Wrap(m.DiffuseMap.WrapS), ["wrapT"] = Wrap(m.DiffuseMap.WrapT) });
                textures.Add(new JsonObject { ["source"] = images.Count - 1, ["sampler"] = samplers.Count - 1 });
                pbr["baseColorTexture"] = new JsonObject { ["index"] = textures.Count - 1 };
                // (see-through where its image is: cut out, or blended where partly so)
                var file = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path)), m.DiffuseMap.FilePath);
                if (File.Exists(file))
                {
                    using var im = SixLabors.ImageSharp.Image.Load<SixLabors.ImageSharp.PixelFormats.Rgba32>(file);
                    bool cut = false, part = false;
                    im.ProcessPixelRows(acc => { for (int y = 0; y < acc.Height; y++) foreach (var px in acc.GetRowSpan(y)) { if (px.A < 250) cut = true; if (px.A > 10 && px.A < 245) part = true; } });
                    if (part) mat["alphaMode"] = "BLEND";
                    else if (cut) { mat["alphaMode"] = "MASK"; mat["alphaCutoff"] = 0.5; }
                }
            }
            mat["pbrMetallicRoughness"] = pbr;
            matIndex[m.Name] = materials.Count;
            materials.Add(mat);
        }

        var meshes = new JsonArray();
        var nodes = new JsonArray();
        foreach (var model in scene.Models)
            foreach (var mesh in model.Meshes)
            {
                var prims = new JsonArray();
                bool hasUV = mesh.HasUVSet(0), hasColor = mesh.HasColorSet(0), hasNormal = mesh.Vertices.Any(v => v.Normal.LengthSquared() > 0.01f);
                foreach (var poly in mesh.Polygons)
                {
                    // each polygon group its own vertices (as used, renumbered)
                    var map = new Dictionary<int, uint>();
                    var idx = new List<uint>();
                    var pos = new List<float>(); var nrm = new List<float>(); var uv = new List<float>(); var col = new List<float>();
                    foreach (int i in poly.Indicies)
                    {
                        if (!map.TryGetValue(i, out uint n))
                        {
                            n = (uint)map.Count; map[i] = n;
                            var v = mesh.Vertices[i];
                            pos.AddRange(new[] { v.Position.X, v.Position.Y, v.Position.Z });
                            if (hasNormal) { var nn = v.Normal.LengthSquared() > 0 ? Vector3.Normalize(v.Normal) : Vector3.UnitY; nrm.AddRange(new[] { nn.X, nn.Y, nn.Z }); }
                            if (hasUV) uv.AddRange(new[] { v.UVs[0].X, 1 - v.UVs[0].Y });  // (glTF: V down, as images)
                            if (hasColor) col.AddRange(new[] { v.Colors[0].X, v.Colors[0].Y, v.Colors[0].Z, v.Colors[0].W });
                        }
                        idx.Add(n);
                    }
                    var attrs = new JsonObject { ["POSITION"] = Floats(pos, 3, "VEC3", true) };
                    if (hasNormal) attrs["NORMAL"] = Floats(nrm, 3, "VEC3", false);
                    if (hasUV) attrs["TEXCOORD_0"] = Floats(uv, 2, "VEC2", false);
                    if (hasColor) attrs["COLOR_0"] = Floats(col, 4, "VEC4", false);
                    var prim = new JsonObject { ["attributes"] = attrs, ["indices"] = Indices(idx), ["mode"] = 4 };
                    if (poly.MaterialName != null && matIndex.TryGetValue(poly.MaterialName, out int mi)) prim["material"] = mi;
                    prims.Add(prim);
                }
                meshes.Add(new JsonObject { ["name"] = mesh.Name, ["primitives"] = prims });
                nodes.Add(new JsonObject { ["name"] = mesh.Name, ["mesh"] = meshes.Count - 1 });
            }

        var binName = Path.GetFileNameWithoutExtension(path) + ".bin";
        File.WriteAllBytes(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path)), binName), bin.ToArray());
        var root = new JsonObject
        {
            ["asset"] = new JsonObject { ["version"] = "2.0", ["generator"] = "KarMapTool" },
            ["scene"] = 0,
            ["scenes"] = new JsonArray { new JsonObject { ["nodes"] = new JsonArray(Enumerable.Range(0, nodes.Count).Select(i => (JsonNode)i).ToArray()) } },
            ["nodes"] = nodes,
            ["meshes"] = meshes,
            ["materials"] = materials,
            ["accessors"] = accessors,
            ["bufferViews"] = bufferViews,
            ["buffers"] = new JsonArray { new JsonObject { ["uri"] = binName, ["byteLength"] = bin.Length } },
        };
        if (images.Count > 0) { root["images"] = images; root["textures"] = textures; root["samplers"] = samplers; }
        File.WriteAllText(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = false, TypeInfoResolver = new System.Text.Json.Serialization.Metadata.DefaultJsonTypeInfoResolver() }));
    }

    // ---- reading ----
    public static IOScene Read(string path)
    {
        string dir = Path.GetDirectoryName(Path.GetFullPath(path));
        JsonNode root;
        byte[] glbBin = null;
        if (path.EndsWith(".glb", StringComparison.OrdinalIgnoreCase))
        {
            var all = File.ReadAllBytes(path);
            int at = 12;
            string json = null;
            while (at + 8 <= all.Length)
            {
                int len = BitConverter.ToInt32(all, at), type = BitConverter.ToInt32(all, at + 4);
                if (type == 0x4E4F534A) json = Encoding.UTF8.GetString(all, at + 8, len);
                else if (type == 0x004E4942) glbBin = all[(at + 8)..(at + 8 + len)];
                at += 8 + len;
            }
            root = JsonNode.Parse(json);
        }
        else root = JsonNode.Parse(File.ReadAllText(path));

        var buffers = (root["buffers"] as JsonArray ?? new JsonArray()).Select(b =>
        {
            var uri = (string)b["uri"];
            if (uri == null) return glbBin;
            if (uri.StartsWith("data:")) return System.Convert.FromBase64String(uri[(uri.IndexOf(',') + 1)..]);
            return File.ReadAllBytes(Path.Combine(dir, Uri.UnescapeDataString(uri)));
        }).ToList();
        var views = root["bufferViews"] as JsonArray;
        var accs = root["accessors"] as JsonArray;
        float[] Read(int ai, out int comps)
        {
            var a = accs[ai];
            string type = (string)a["type"];
            comps = type switch { "SCALAR" => 1, "VEC2" => 2, "VEC3" => 3, "VEC4" => 4, _ => 1 };
            int count = (int)a["count"], ct = (int)a["componentType"];
            bool norm = (bool?)a["normalized"] ?? false;
            var res = new float[count * comps];
            if (a["bufferView"] == null) return res;
            var v = views[(int)a["bufferView"]];
            var buf = buffers[(int)v["buffer"]];
            int off = ((int?)v["byteOffset"] ?? 0) + ((int?)a["byteOffset"] ?? 0);
            int csize = ct switch { FLOAT => 4, UINT => 4, USHORT => 2, 5122 => 2, _ => 1 };
            int stride = (int?)v["byteStride"] ?? csize * comps;
            for (int i = 0; i < count; i++)
                for (int c = 0; c < comps; c++)
                {
                    int p = off + i * stride + c * csize;
                    res[i * comps + c] = ct switch
                    {
                        FLOAT => BitConverter.ToSingle(buf, p),
                        UINT => BitConverter.ToUInt32(buf, p),
                        USHORT => norm ? BitConverter.ToUInt16(buf, p) / 65535f : BitConverter.ToUInt16(buf, p),
                        5122 => norm ? Math.Max(-1, BitConverter.ToInt16(buf, p) / 32767f) : BitConverter.ToInt16(buf, p),
                        UBYTE => norm ? buf[p] / 255f : buf[p],
                        _ => norm ? Math.Max(-1, (sbyte)buf[p] / 127f) : (sbyte)buf[p],
                    };
                }
            return res;
        }

        var scene = new IOScene();
        var images = root["images"] as JsonArray;
        var textures = root["textures"] as JsonArray;
        var samplers = root["samplers"] as JsonArray;
        var matNames = new List<string>();
        static WrapMode WrapOf(int? w) => w == CLAMP ? WrapMode.CLAMP : w == MIRROR ? WrapMode.MIRROR : WrapMode.REPEAT;
        int mi = 0;
        foreach (var m in root["materials"] as JsonArray ?? new JsonArray())
        {
            string name = (string)m["name"] ?? $"material{mi}";
            if (matNames.Contains(name)) name += $"_{mi}";
            mi++;
            matNames.Add(name);
            var mat = new IOMaterial { Name = name };
            var bt = m["pbrMetallicRoughness"]?["baseColorTexture"];
            if (bt != null && textures != null)
            {
                var t = textures[(int)bt["index"]];
                var img = images[(int)t["source"]];
                var tex = new IOTexture { Name = name };
                if (img["uri"] is JsonNode u && !((string)u).StartsWith("data:")) tex.FilePath = Path.Combine(dir, Uri.UnescapeDataString((string)u));
                else
                {
                    // an embedded image (a .glb's, or a data URI): handed over as its bytes
                    byte[] data;
                    if (img["uri"] is JsonNode du) data = System.Convert.FromBase64String(((string)du)[(((string)du).IndexOf(',') + 1)..]);
                    else
                    {
                        var v = views[(int)img["bufferView"]];
                        int off = (int?)v["byteOffset"] ?? 0, len = (int)v["byteLength"];
                        data = buffers[(int)v["buffer"]][off..(off + len)];
                    }
                    tex.FileBlob = data;
                    tex.Type = ((string)img["mimeType"])?.Contains("jpeg") == true ? IONET.Core.Model.ImageFileType.JPEG : IONET.Core.Model.ImageFileType.PNG;
                }
                if (t["sampler"] != null && samplers != null)
                {
                    var s = samplers[(int)t["sampler"]];
                    tex.WrapS = WrapOf((int?)s["wrapS"]);
                    tex.WrapT = WrapOf((int?)s["wrapT"]);
                }
                mat.DiffuseMap = tex;
            }
            scene.Materials.Add(mat);
        }

        var model = new IOModel { Name = Path.GetFileNameWithoutExtension(path) };
        scene.Models.Add(model);
        var nodes = root["nodes"] as JsonArray ?? new JsonArray();
        var meshes = root["meshes"] as JsonArray ?? new JsonArray();
        Matrix4x4 Local(JsonNode n)
        {
            if (n["matrix"] is JsonArray ma)
            {
                var f = ma.Select(x => (float)x).ToArray();
                return new Matrix4x4(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            }
            var tr = n["translation"] is JsonArray ta ? new Vector3((float)ta[0], (float)ta[1], (float)ta[2]) : Vector3.Zero;
            var ro = n["rotation"] is JsonArray ra ? new Quaternion((float)ra[0], (float)ra[1], (float)ra[2], (float)ra[3]) : Quaternion.Identity;
            var sc = n["scale"] is JsonArray sa ? new Vector3((float)sa[0], (float)sa[1], (float)sa[2]) : Vector3.One;
            return Matrix4x4.CreateScale(sc) * Matrix4x4.CreateFromQuaternion(ro) * Matrix4x4.CreateTranslation(tr);
        }
        void Visit(int ni, Matrix4x4 parent)
        {
            var n = nodes[ni];
            var world = Local(n) * parent;
            if (n["mesh"] != null)
            {
                var gm = meshes[(int)n["mesh"]];
                var mesh = new IOMesh { Name = (string)n["name"] ?? (string)gm["name"] ?? $"mesh{model.Meshes.Count}" };
                Matrix4x4.Invert(world, out var inv);
                var nmat = Matrix4x4.Transpose(inv);
                foreach (var p in gm["primitives"] as JsonArray)
                {
                    int mode = (int?)p["mode"] ?? 4;
                    if (mode != 4) continue;  // (triangles only)
                    var at = p["attributes"];
                    var pos = Read((int)at["POSITION"], out _);
                    float[] nrm = at["NORMAL"] != null ? Read((int)at["NORMAL"], out _) : null;
                    float[] uv = at["TEXCOORD_0"] != null ? Read((int)at["TEXCOORD_0"], out _) : null;
                    int cc = 0;
                    float[] col = at["COLOR_0"] != null ? Read((int)at["COLOR_0"], out cc) : null;
                    int nv = pos.Length / 3;
                    int baseV = mesh.Vertices.Count;
                    for (int i = 0; i < nv; i++)
                    {
                        var v = new IOVertex { Position = Vector3.Transform(new Vector3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]), world) };
                        if (nrm != null) v.Normal = Vector3.Normalize(Vector3.TransformNormal(new Vector3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]), nmat));
                        if (uv != null) v.SetUV(uv[i * 2], 1 - uv[i * 2 + 1], 0);
                        if (col != null) v.SetColor(col[i * cc], col[i * cc + 1], col[i * cc + 2], cc == 4 ? col[i * cc + 3] : 1, 0);
                        mesh.Vertices.Add(v);
                    }
                    var poly = new IOPolygon { PrimitiveType = IOPrimitive.TRIANGLE };
                    if (p["material"] != null) poly.MaterialName = matNames[(int)p["material"]];
                    if (p["indices"] != null) foreach (var x in Read((int)p["indices"], out _)) poly.Indicies.Add(baseV + (int)x);
                    else for (int i = 0; i < nv; i++) poly.Indicies.Add(baseV + i);
                    mesh.Polygons.Add(poly);
                }
                if (mesh.Polygons.Count > 0) model.Meshes.Add(mesh);
            }
            if (n["children"] is JsonArray ch) foreach (var c in ch) Visit((int)c, world);
        }
        var sceneRoots = root["scenes"]?[(int?)root["scene"] ?? 0]?["nodes"] as JsonArray;
        if (sceneRoots != null) foreach (var r in sceneRoots) Visit((int)r, Matrix4x4.Identity);
        else for (int i = 0; i < nodes.Count; i++) Visit(i, Matrix4x4.Identity);
        return scene;
    }

    // The map's objects placed in the model's file (Blender: empties named KAR_Spawn..., KAR_Area..., KAR_Warp...; their
    // custom properties - music, to, loop_start - come as the nodes' extras): each its name, place, turn about the up
    // axis (degrees), size (an empty's cube: twice its scale) and properties.
    public record Marker(string Name, Vector3 Position, float Yaw, Vector3 Size, JsonObject Extras);

    public static List<Marker> Markers(string path)
    {
        var list = new List<Marker>();
        if (!path.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase)) return list;  // (.glb: the same, when needed)
        var root = JsonNode.Parse(File.ReadAllText(path));
        var nodes = root["nodes"] as JsonArray ?? new JsonArray();
        void Visit(int ni, Matrix4x4 parent)
        {
            var n = nodes[ni];
            var tr = n["translation"] is JsonArray ta ? new Vector3((float)ta[0], (float)ta[1], (float)ta[2]) : Vector3.Zero;
            var ro = n["rotation"] is JsonArray ra ? new Quaternion((float)ra[0], (float)ra[1], (float)ra[2], (float)ra[3]) : Quaternion.Identity;
            var sc = n["scale"] is JsonArray sa ? new Vector3((float)sa[0], (float)sa[1], (float)sa[2]) : Vector3.One;
            var world = Matrix4x4.CreateScale(sc) * Matrix4x4.CreateFromQuaternion(ro) * Matrix4x4.CreateTranslation(tr) * parent;
            string name = (string)n["name"] ?? "";
            if (n["mesh"] == null && name.StartsWith("KAR_", StringComparison.OrdinalIgnoreCase))
            {
                Matrix4x4.Decompose(world, out var s, out var q, out var t);
                // facing: where the node's -Z points, about the up axis (0: facing -Z, as the stage's start positions)
                var f = Vector3.Transform(-Vector3.UnitZ, q);
                float yaw = MathF.Atan2(-f.X, -f.Z) * 180f / MathF.PI;
                list.Add(new Marker(name, t, yaw, Vector3.Abs(s) * 2, n["extras"] as JsonObject ?? new JsonObject()));
            }
            if (n["children"] is JsonArray ch) foreach (var c in ch) Visit((int)c, world);
        }
        var sceneRoots = root["scenes"]?[(int?)root["scene"] ?? 0]?["nodes"] as JsonArray;
        if (sceneRoots != null) foreach (var r in sceneRoots) Visit((int)r, Matrix4x4.Identity);
        return list;
    }

    // Any model file: glTF here, the rest by IONET.
    public static IOScene Load(string path)
    {
        if (path.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase) || path.EndsWith(".glb", StringComparison.OrdinalIgnoreCase)) return Read(path);
        return IONET.IOManager.LoadScene(path, new IONET.ImportSettings { Triangulate = true });
    }
}
