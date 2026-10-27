// The 3D view's contents: the map's model (textured, its vertex colours) and what is placed on it, drawn with the
// viewer's camera; clicking picks an object, Alt+drag moves it over the ground.
using HSDRawViewer.Rendering;
using IONET.Core;
using OpenTK.Graphics.OpenGL;
using OpenTK.Mathematics;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using Color = System.Drawing.Color;
using NVec3 = System.Numerics.Vector3;

namespace KarMapEditor;

public class MapView : IDrawableInterface
{
    public DrawOrder DrawOrder => DrawOrder.First;

    readonly MapDoc _doc;
    MapSpec _spec => _doc.Spec;
    public MapView(MapDoc doc) { _doc = doc; doc.GroundBelow = GroundBelow; }
    bool _dirty;
    // the model: one batch of triangles per material (position, uv, colour: 9 floats a vertex)
    class Batch { public float[] Data; public int Count; public string Texture; public bool Clamp; public int Vbo; public int Tex; }
    readonly List<Batch> _batches = new();
    readonly Dictionary<string, int> _textures = new();
    float[] _tris = Array.Empty<float>();  // every triangle's corners, for picking the ground
    public NVec3 Min { get; private set; }
    public NVec3 Max { get; private set; }

    object Selected => _doc.Selected;
    public bool ShowAreas { get; set; } = true;
    public bool ShowWarps { get; set; } = true;
    public bool ShowSpawns { get; set; } = true;
    public bool ShowEvents { get; set; } = true;

    public void Load(IOScene scene)
    {
        _dirty = true;
        var tris = new List<float>();
        var min = new NVec3(float.MaxValue); var max = new NVec3(float.MinValue);
        _pending.Clear();
        if (scene != null && scene.Models.Count > 0)
        {
            var mats = scene.Materials.ToDictionary(m => m.Name, m => m);
            var byMat = new Dictionary<string, List<float>>();
            foreach (var mesh in scene.Models[0].Meshes)
                foreach (var poly in mesh.Polygons)
                {
                    var key = poly.MaterialName ?? "";
                    if (!byMat.TryGetValue(key, out var list)) byMat[key] = list = new List<float>();
                    var ix = poly.Indicies;
                    for (int i = 0; i + 2 < ix.Count; i += 3)
                        for (int k = 0; k < 3; k++)
                        {
                            var v = mesh.Vertices[ix[i + k]];
                            var uv = v.UVs.Count > 0 ? v.UVs[0] : System.Numerics.Vector2.Zero;
                            var c = v.Colors.Count > 0 ? v.Colors[0] : System.Numerics.Vector4.One;
                            list.AddRange(new[] { v.Position.X, v.Position.Y, v.Position.Z, uv.X, uv.Y, c.X, c.Y, c.Z, c.W });
                            tris.Add(v.Position.X); tris.Add(v.Position.Y); tris.Add(v.Position.Z);
                            min = NVec3.Min(min, v.Position); max = NVec3.Max(max, v.Position);
                        }
                }
            foreach (var (name, data) in byMat)
            {
                mats.TryGetValue(name, out var mt);
                var tex = mt?.DiffuseMap;
                _pending.Add(new Batch
                {
                    Data = data.ToArray(), Count = data.Count / 9, Texture = tex?.FilePath,
                    Clamp = tex != null && tex.WrapS == IONET.Core.Model.WrapMode.CLAMP,
                });
            }
        }
        _tris = tris.ToArray();
        if (min.X > max.X) { min = new NVec3(-1000); max = new NVec3(1000); }
        Min = min; Max = max;
    }
    readonly List<Batch> _pending = new();

    public void GLInit() { }

    public void GLFree()
    {
        foreach (var b in _batches) if (b.Vbo != 0) GL.DeleteBuffer(b.Vbo);
        foreach (var t in _textures.Values) GL.DeleteTexture(t);
        _batches.Clear(); _textures.Clear();
        _skyKey = null;
    }

    void Upload()
    {
        GLFree();
        foreach (var b in _pending)
        {
            b.Vbo = GL.GenBuffer();
            GL.BindBuffer(BufferTarget.ArrayBuffer, b.Vbo);
            GL.BufferData(BufferTarget.ArrayBuffer, b.Data.Length * 4, b.Data, BufferUsageHint.StaticDraw);
            b.Tex = b.Texture != null ? Texture(b.Texture, b.Clamp) : 0;
            _batches.Add(b);
        }
        GL.BindBuffer(BufferTarget.ArrayBuffer, 0);
        // (opaque first, then the cut-out and see-through: those drawn last)
        _dirty = false;
    }

    int Texture(string path, bool clamp)
    {
        var key = path + (clamp ? "|c" : "");
        if (_textures.TryGetValue(key, out int t)) return t;
        t = 0;
        try
        {
            using var img = SixLabors.ImageSharp.Image.Load<Rgba32>(path);
            var px = new byte[img.Width * img.Height * 4];
            img.CopyPixelDataTo(px);
            t = GL.GenTexture();
            GL.BindTexture(TextureTarget.Texture2D, t);
            GL.TexImage2D(TextureTarget.Texture2D, 0, PixelInternalFormat.Rgba, img.Width, img.Height, 0, PixelFormat.Rgba, PixelType.UnsignedByte, px);
            GL.GenerateMipmap(GenerateMipmapTarget.Texture2D);
            GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)TextureMinFilter.LinearMipmapLinear);
            GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Linear);
            var wrap = clamp ? (int)TextureWrapMode.ClampToEdge : (int)TextureWrapMode.Repeat;
            GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, wrap);
            GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, wrap);
        }
        catch { }
        _textures[key] = t;
        return t;
    }

    // ---- the sky: the map's panorama round the camera (its bottom edge the horizon), its overhead picture above ----
    int _skyPano, _skyTop;
    float _skyAspect = 0.25f;  // (the panorama's height over its width)
    string _skyKey;

    void SkyTextures()
    {
        var key = $"{_spec?.SkyPanorama}|{_spec?.SkyTop}";
        if (key == _skyKey) return;
        _skyKey = key;
        _skyPano = _spec?.SkyPanorama != null && File.Exists(_spec.SkyPanorama) ? Texture(_spec.SkyPanorama, false) : 0;
        _skyTop = _spec?.SkyTop != null && File.Exists(_spec.SkyTop) ? Texture(_spec.SkyTop, true) : 0;
        if (_skyPano != 0)
            try { var info = SixLabors.ImageSharp.Image.Identify(_spec.SkyPanorama); _skyAspect = (float)info.Height / info.Width; } catch { }
    }

    void DrawSky(Camera cam)
    {
        SkyTextures();
        if (_skyPano == 0 && _skyTop == 0) return;
        var eye = cam.ModelViewMatrix.Inverted().Row3.Xyz;
        float r = 40000, h = r * MathF.PI * 2 * _skyAspect;
        GL.PushAttrib(AttribMask.AllAttribBits);
        GL.Disable(EnableCap.DepthTest);
        GL.DepthMask(false);
        GL.Disable(EnableCap.CullFace);
        GL.Disable(EnableCap.Lighting);
        GL.Disable(EnableCap.AlphaTest);
        GL.Enable(EnableCap.Texture2D);
        GL.Color4(1f, 1f, 1f, 1f);
        GL.MatrixMode(MatrixMode.Modelview);
        GL.PushMatrix();
        GL.Translate(eye.X, eye.Y, eye.Z);
        const int seg = 64;
        if (_skyPano != 0)
        {
            GL.BindTexture(TextureTarget.Texture2D, _skyPano);
            GL.Begin(PrimitiveType.QuadStrip);
            for (int i = 0; i <= seg; i++)
            {
                float a = i * MathF.PI * 2 / seg, u = (float)i / seg;
                float x = MathF.Sin(a) * r, z = -MathF.Cos(a) * r;
                GL.TexCoord2(u, 1f); GL.Vertex3(x, -r * 0.02f, z);
                GL.TexCoord2(u, 0f); GL.Vertex3(x, h, z);
            }
            GL.End();
            // below the horizon: its colour, down to the ground
            GL.Begin(PrimitiveType.QuadStrip);
            for (int i = 0; i <= seg; i++)
            {
                float a2 = i * MathF.PI * 2 / seg, u = (float)i / seg;
                float x = MathF.Sin(a2) * r, z = -MathF.Cos(a2) * r;
                GL.TexCoord2(u, 0.995f); GL.Vertex3(x, -r, z);
                GL.TexCoord2(u, 0.995f); GL.Vertex3(x, -r * 0.02f, z);
            }
            GL.End();
            GL.Begin(PrimitiveType.TriangleFan);
            GL.TexCoord2(0.5f, 0.995f); GL.Vertex3(0, -r, 0);
            for (int i = 0; i <= seg; i++)
            {
                float a3 = i * MathF.PI * 2 / seg;
                GL.TexCoord2((float)i / seg, 0.995f); GL.Vertex3(MathF.Sin(a3) * r, -r, -MathF.Cos(a3) * r);
            }
            GL.End();
        }
        if (_skyTop != 0)
        {
            GL.BindTexture(TextureTarget.Texture2D, _skyTop);
            GL.Begin(PrimitiveType.TriangleFan);
            GL.TexCoord2(0.5f, 0.5f); GL.Vertex3(0, h, 0);
            for (int i = 0; i <= seg; i++)
            {
                float a = i * MathF.PI * 2 / seg;
                GL.TexCoord2(0.5f + MathF.Sin(a) * 0.5f, 0.5f - MathF.Cos(a) * 0.5f);
                GL.Vertex3(MathF.Sin(a) * r, h, -MathF.Cos(a) * r);
            }
            GL.End();
        }
        GL.PopMatrix();
        GL.PopAttrib();
    }

    public void Draw(Camera cam, int windowWidth, int windowHeight)
    {
        GL.UseProgram(0);
        if (_dirty) Upload();
        DrawSky(cam);
        GL.PushAttrib(AttribMask.AllAttribBits);
        GL.Disable(EnableCap.Lighting);
        GL.Disable(EnableCap.CullFace);
        GL.Enable(EnableCap.DepthTest);
        GL.Enable(EnableCap.AlphaTest);
        GL.AlphaFunc(AlphaFunction.Greater, 0.3f);
        GL.Enable(EnableCap.Blend);
        GL.BlendFunc(BlendingFactor.SrcAlpha, BlendingFactor.OneMinusSrcAlpha);
        GL.TexEnv(TextureEnvTarget.TextureEnv, TextureEnvParameter.TextureEnvMode, (int)TextureEnvMode.Modulate);
        GL.EnableClientState(ArrayCap.VertexArray);
        GL.EnableClientState(ArrayCap.TextureCoordArray);
        GL.EnableClientState(ArrayCap.ColorArray);
        foreach (var b in _batches)
        {
            if (b.Tex != 0) { GL.Enable(EnableCap.Texture2D); GL.BindTexture(TextureTarget.Texture2D, b.Tex); }
            else GL.Disable(EnableCap.Texture2D);
            GL.BindBuffer(BufferTarget.ArrayBuffer, b.Vbo);
            GL.VertexPointer(3, VertexPointerType.Float, 36, 0);
            GL.TexCoordPointer(2, TexCoordPointerType.Float, 36, 12);
            GL.ColorPointer(4, ColorPointerType.Float, 36, 20);
            GL.DrawArrays(PrimitiveType.Triangles, 0, b.Count);
        }
        GL.BindBuffer(BufferTarget.ArrayBuffer, 0);
        GL.DisableClientState(ArrayCap.VertexArray);
        GL.DisableClientState(ArrayCap.TextureCoordArray);
        GL.DisableClientState(ArrayCap.ColorArray);
        GL.Disable(EnableCap.Texture2D);
        GL.Disable(EnableCap.AlphaTest);
        GL.PopAttrib();

        if (_spec == null) return;
        // what is placed: over the model (seen through it)
        GL.PushAttrib(AttribMask.AllAttribBits);
        GL.Disable(EnableCap.DepthTest);
        GL.Disable(EnableCap.Texture2D);
        GL.Disable(EnableCap.Lighting);
        if (ShowAreas) foreach (var a in _spec.Areas) DrawBox(a, a == Selected ? Color.White : Theme.Area, filled: a == Selected);
        if (ShowWarps)
            foreach (var w in _spec.Warps)
            {
                var col = w == Selected ? Color.White : Theme.Warp;
                DrawBox(w, col, filled: true);
                var (to, toYaw) = _doc.Destination(w);
                Line(w.Position, to, w == Selected ? Color.White : Color.FromArgb(120, 160, 255));
                Marker(to, toYaw, 25, w == Selected ? Color.White : Theme.WarpTo);
            }
        if (ShowSpawns) foreach (var s in _spec.Spawns) Marker(s.Position, s.Yaw, 20, s == Selected ? Color.White : Theme.Spawn);
        if (ShowEvents)
            foreach (var e in _spec.EventPoints)
            {
                var c = e == Selected ? Color.White : Theme.Event;
                Marker(e.Position, e.Yaw, 16, c);
                DrawShape.DrawBox(c, BoxMatrix(e), -6, 20, -6, 6, 60, 6);
            }
        GL.PopAttrib();
    }

    static NVec3 Forward(float yawDeg)
    {
        float r = MathHelper.DegreesToRadians(yawDeg);
        return new NVec3(-MathF.Sin(r), 0, -MathF.Cos(r));
    }

    static Matrix4 BoxMatrix(Placed p) => Matrix4.CreateRotationY(MathHelper.DegreesToRadians(p.Yaw)) * Matrix4.CreateTranslation(p.X, p.Y, p.Z);

    static void DrawBox(Box b, Color c, bool filled)
    {
        float hx = b.SizeX / 2, hy = b.SizeY / 2, hz = b.SizeZ / 2;
        var m = BoxMatrix(b);
        GL.MatrixMode(MatrixMode.Modelview);
        GL.PushMatrix();
        GL.MultMatrix(ref m);
        // see-through sides, then the edges
        GL.Enable(EnableCap.Blend);
        GL.BlendFunc(BlendingFactor.SrcAlpha, BlendingFactor.OneMinusSrcAlpha);
        GL.Color4(c.R / 255f, c.G / 255f, c.B / 255f, filled ? 0.15f : 0f);
        GL.Begin(PrimitiveType.Quads);
        void Q(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float dx, float dy, float dz)
        { GL.Vertex3(ax, ay, az); GL.Vertex3(bx, by, bz); GL.Vertex3(cx, cy, cz); GL.Vertex3(dx, dy, dz); }
        Q(-hx, -hy, -hz, hx, -hy, -hz, hx, hy, -hz, -hx, hy, -hz);
        Q(-hx, -hy, hz, hx, -hy, hz, hx, hy, hz, -hx, hy, hz);
        Q(-hx, -hy, -hz, -hx, -hy, hz, -hx, hy, hz, -hx, hy, -hz);
        Q(hx, -hy, -hz, hx, -hy, hz, hx, hy, hz, hx, hy, -hz);
        Q(-hx, hy, -hz, hx, hy, -hz, hx, hy, hz, -hx, hy, hz);
        Q(-hx, -hy, -hz, hx, -hy, -hz, hx, -hy, hz, -hx, -hy, hz);
        GL.End();
        GL.PopMatrix();
        DrawShape.DrawBox(c, m, -hx, -hy, -hz, hx, hy, hz);
        // its facing
        GL.PushMatrix();
        GL.MultMatrix(ref m);
        GL.LineWidth(2);
        GL.Color3(c);
        GL.Begin(PrimitiveType.Lines);
        GL.Vertex3(0, 0, 0); GL.Vertex3(0, 0, -hz - 40);
        GL.End();
        GL.PopMatrix();
    }

    static void Marker(NVec3 p, float yaw, float size, Color c)
    {
        var m = BoxMatrix(new Placed { X = p.X, Y = p.Y, Z = p.Z, Yaw = yaw });
        DrawShape.DrawBox(c, m, -size / 2, 0, -size / 2, size / 2, size, size / 2);
        GL.MatrixMode(MatrixMode.Modelview);
        GL.PushMatrix();
        GL.MultMatrix(ref m);
        GL.LineWidth(3);
        GL.Color3(c);
        GL.Begin(PrimitiveType.Lines);
        GL.Vertex3(0, size / 2, 0); GL.Vertex3(0, size / 2, -size * 2.5f);
        GL.Vertex3(0, size / 2, -size * 2.5f); GL.Vertex3(-size / 2, size / 2, -size * 1.8f);
        GL.Vertex3(0, size / 2, -size * 2.5f); GL.Vertex3(size / 2, size / 2, -size * 1.8f);
        GL.End();
        GL.PopMatrix();
    }

    static void Line(NVec3 a, NVec3 b, Color c)
    {
        GL.LineWidth(1.5f);
        GL.Enable(EnableCap.LineStipple);
        GL.LineStipple(4, 0xAAAA);
        GL.Color3(c);
        GL.Begin(PrimitiveType.Lines);
        GL.Vertex3(a.X, a.Y, a.Z); GL.Vertex3(b.X, b.Y, b.Z);
        GL.End();
        GL.Disable(EnableCap.LineStipple);
    }

    // ---- picking ----
    static (NVec3 origin, NVec3 dir) Ray(PickInformation pick)
    {
        float x = 2f * pick.ScreenPoint.X / pick.ViewportWidth - 1f;
        float y = 1f - 2f * pick.ScreenPoint.Y / pick.ViewportHeight;
        var inv = pick.Camera.MvpMatrix.Inverted();
        var a = new Vector4(x, y, -1, 1) * inv; var b = new Vector4(x, y, 1, 1) * inv;
        var na = a.Xyz / a.W; var fb = b.Xyz / b.W;
        var o = new NVec3(na.X, na.Y, na.Z);
        return (o, NVec3.Normalize(new NVec3(fb.X, fb.Y, fb.Z) - o));
    }

    // the nearest of the model's triangles along a ray
    public bool Raycast(NVec3 o, NVec3 d, out NVec3 hit)
    {
        float best = float.MaxValue;
        var t = _tris;
        for (int i = 0; i + 8 < t.Length; i += 9)
        {
            var a = new NVec3(t[i], t[i + 1], t[i + 2]);
            var e1 = new NVec3(t[i + 3], t[i + 4], t[i + 5]) - a;
            var e2 = new NVec3(t[i + 6], t[i + 7], t[i + 8]) - a;
            var p = NVec3.Cross(d, e2);
            float det = NVec3.Dot(e1, p);
            if (MathF.Abs(det) < 1e-8f) continue;
            float inv = 1 / det;
            var s = o - a;
            float u = NVec3.Dot(s, p) * inv;
            if (u < 0 || u > 1) continue;
            var q = NVec3.Cross(s, e1);
            float v = NVec3.Dot(d, q) * inv;
            if (v < 0 || u + v > 1) continue;
            float dist = NVec3.Dot(e2, q) * inv;
            if (dist > 0 && dist < best) best = dist;
        }
        hit = o + d * (best == float.MaxValue ? 0 : best);
        return best != float.MaxValue;
    }

    public float GroundBelow(NVec3 p, float fallback)
    {
        return Raycast(new NVec3(p.X, Max.Y + 1000, p.Z), -NVec3.UnitY, out var h) ? h.Y : fallback;
    }

    // a ray against a turned box: its distance, or none
    static float? HitBox(NVec3 o, NVec3 d, Placed p, NVec3 half, NVec3 centreOffset)
    {
        float r = -MathHelper.DegreesToRadians(p.Yaw);
        NVec3 Local(NVec3 v) => new(v.X * MathF.Cos(r) + v.Z * MathF.Sin(r), v.Y, -v.X * MathF.Sin(r) + v.Z * MathF.Cos(r));
        var lo = Local(o - p.Position) - centreOffset; var ld = Local(d);
        float tmin = 0, tmax = float.MaxValue;
        for (int k = 0; k < 3; k++)
        {
            float oo = k == 0 ? lo.X : k == 1 ? lo.Y : lo.Z, dd = k == 0 ? ld.X : k == 1 ? ld.Y : ld.Z, h = k == 0 ? half.X : k == 1 ? half.Y : half.Z;
            if (MathF.Abs(dd) < 1e-8f) { if (oo < -h || oo > h) return null; continue; }
            float t1 = (-h - oo) / dd, t2 = (h - oo) / dd;
            if (t1 > t2) (t1, t2) = (t2, t1);
            tmin = Math.Max(tmin, t1); tmax = Math.Min(tmax, t2);
            if (tmin > tmax) return null;
        }
        return tmin;
    }

    public object Pick(PickInformation pick)
    {
        if (_spec == null) return null;
        var (o, d) = Ray(pick);
        object best = null; float bestT = float.MaxValue;
        void Try(object obj, float? t, float bias)
        {
            if (t is float v && v + bias < bestT) { bestT = v + bias; best = obj; }
        }
        // (small things first: a spawn or a warp inside a big area wins)
        if (ShowSpawns) foreach (var s in _spec.Spawns) Try(s, HitBox(o, d, s, new NVec3(15, 15, 15), new NVec3(0, 10, 0)), 0);
        if (ShowWarps) foreach (var w in _spec.Warps) Try(w, HitBox(o, d, w, new NVec3(w.SizeX / 2, w.SizeY / 2, w.SizeZ / 2), NVec3.Zero), 0);
        if (ShowEvents) foreach (var e in _spec.EventPoints) Try(e, HitBox(o, d, e, new NVec3(12, 30, 12), new NVec3(0, 30, 0)), 0);
        if (ShowAreas) foreach (var a in _spec.Areas) Try(a, HitBox(o, d, a, new NVec3(a.SizeX / 2, a.SizeY / 2, a.SizeZ / 2), NVec3.Zero), 1e6f);
        return best;
    }

    public NVec3? GroundAt(PickInformation pick)
    {
        var (o, d) = Ray(pick);
        return Raycast(o, d, out var h) ? h : null;
    }

    // ---- the viewport's input: the tool in hand ----
    public void ScreenClick(MouseButtons button, PickInformation pick)
    {
        bool moved = _moved; _moved = false;
        _dragging = false; _camera = false;
        if (button != MouseButtons.Left || moved || _doc.Spec == null) return;  // (the end of turning the view, or of moving something)
        if (_doc.Tool is Tool.Spawn or Tool.Area or Tool.Warp or Tool.Event)
        {
            if (GroundAt(pick) is NVec3 g) _doc.Place(_doc.Tool, g);
            return;
        }
        _doc.Selected = Pick(pick);
    }

    public event Action<NVec3> GroundDoubleClicked;
    public void ScreenDoubleClick(PickInformation pick)
    {
        if (GroundAt(pick) is NVec3 g) GroundDoubleClicked?.Invoke(g);
    }

    // A drag that starts on the picked object (or any with Alt) works it with the tool - moves it over the ground, turns
    // it, sizes it; one that starts elsewhere turns the view.
    bool _dragging, _camera, _moved;
    float _dragHeight;
    public void ScreenDrag(MouseEventArgs args, PickInformation pick, float deltaX, float deltaY)
    {
        if (args.Button == MouseButtons.None) { _dragging = false; _camera = false; return; }
        if (deltaX != 0 || deltaY != 0) _moved = true;
        if (args.Button != MouseButtons.Left || _doc.Spec == null || _camera) return;
        if (!_dragging)
        {
            bool alt = (Control.ModifierKeys & Keys.Alt) != 0;
            var hit = Pick(pick);
            if (hit == null || (!alt && hit != _doc.Selected) || _doc.Tool is Tool.Spawn or Tool.Area or Tool.Warp or Tool.Event) { _camera = true; return; }
            _doc.Selected = hit;
            var p0 = (Placed)hit;
            _dragging = true;
            _dragHeight = p0.Y - GroundBelow(p0.Position, p0.Y);
            _doc.Checkpoint();
        }
        if (_doc.Selected is not Placed p) return;
        switch (_doc.Tool)
        {
            case Tool.Rotate:
                p.Yaw = ((p.Yaw - deltaX * 0.02f) % 360 + 360) % 360;
                break;
            case Tool.Scale when p is Box b:
                float f = MathF.Max(0.5f, 1 + deltaY * 0.0004f);
                b.SizeX = MathF.Max(10, b.SizeX * f); b.SizeY = MathF.Max(10, b.SizeY * f); b.SizeZ = MathF.Max(10, b.SizeZ * f);
                break;
            default:
                if (GroundAt(pick) is NVec3 g) { p.X = _doc.SnapTo(g.X); p.Z = _doc.SnapTo(g.Z); p.Y = GroundBelow(new NVec3(p.X, g.Y + 50, p.Z), g.Y) + _dragHeight; }
                break;
        }
        _doc.Touch();
    }

    public void ScreenSelectArea(PickInformation start, PickInformation end) { }

    public bool FreezeCamera() => _dragging || (Control.ModifierKeys & Keys.Alt) != 0;

    public event Action<KeyEventArgs> KeyPressed;
    public void ViewportKeyPress(KeyEventArgs kbState) => KeyPressed?.Invoke(kbState);
}
