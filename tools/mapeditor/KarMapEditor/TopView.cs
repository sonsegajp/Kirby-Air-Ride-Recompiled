// The view from above (as Hammer's "Top" view): the map drawn from overhead on a grid, what is placed on it as shapes.
// Wheel: zoom at the cursor; right or middle drag: slide. With the select tools: click picks, drag moves (on the grid
// when snapping), a box's square handles resize it, the round one above it turns it. With an object tool: click places.
using System.Drawing.Drawing2D;
using SixLabors.ImageSharp.PixelFormats;
using NVec3 = System.Numerics.Vector3;

namespace KarMapEditor;

public class TopView : Control
{
    readonly MapDoc _doc;
    Bitmap _map;                 // the map from overhead
    RectangleF _mapRect;         // ... its extent in the world (X, Z)
    float _zoom = 0.1f;          // pixels per unit
    PointF _centre;              // the world point (X, Z) in the middle of the view
    public event Action<NVec3> CursorMoved;
    public bool Busy { get; set; }

    public TopView(MapDoc doc)
    {
        _doc = doc;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
        BackColor = Color.FromArgb(28, 28, 30);
        doc.Changed += Invalidate;
        doc.SelectionChanged += Invalidate;
        doc.StructureChanged += Invalidate;
    }

    // the overhead picture of the model (made off the window's thread: a big map takes a few seconds)
    public void SetMap(SixLabors.ImageSharp.Image<Rgba32> img, RectangleF worldRect)
    {
        var bmp = new Bitmap(img.Width, img.Height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        var data = bmp.LockBits(new Rectangle(0, 0, img.Width, img.Height), System.Drawing.Imaging.ImageLockMode.WriteOnly, bmp.PixelFormat);
        var px = new byte[img.Width * img.Height * 4];
        img.CloneAs<Bgra32>().CopyPixelDataTo(px);
        System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
        bmp.UnlockBits(data);
        _map?.Dispose();
        _map = bmp;
        _mapRect = worldRect;
        Invalidate();
    }

    public void FrameAll()
    {
        var r = _mapRect;
        if (r.Width <= 0)
        {
            var pts = _doc.All().ToList();
            if (pts.Count == 0) return;
            float x0 = pts.Min(p => p.X), x1 = pts.Max(p => p.X), z0 = pts.Min(p => p.Z), z1 = pts.Max(p => p.Z);
            r = RectangleF.FromLTRB(x0 - 200, z0 - 200, x1 + 200, z1 + 200);
        }
        _centre = new PointF(r.X + r.Width / 2, r.Y + r.Height / 2);
        _zoom = Math.Min(Width / Math.Max(1f, r.Width), Height / Math.Max(1f, r.Height)) * 0.92f;
        Invalidate();
    }

    public void FrameOn(Placed p)
    {
        _centre = new PointF(p.X, p.Z);
        if (p is Box b) _zoom = Math.Min(Width, Height) / Math.Max(200f, Math.Max(b.SizeX, b.SizeZ) * 2.2f);
        Invalidate();
    }

    PointF ToScreen(float x, float z) => new((x - _centre.X) * _zoom + Width / 2f, (z - _centre.Y) * _zoom + Height / 2f);
    PointF ToWorld(Point s) => new((s.X - Width / 2f) / _zoom + _centre.X, (s.Y - Height / 2f) / _zoom + _centre.Y);

    // a box's corners (local +-x, +-z turned by its yaw) in the world
    static PointF Local(Box b, float lx, float lz)
    {
        float r = b.Yaw * MathF.PI / 180f, c = MathF.Cos(r), s = MathF.Sin(r);
        // (the game's turn: x' = x cos + z sin, z' = -x sin + z cos)
        return new PointF(b.X + lx * c + lz * s, b.Z - lx * s + lz * c);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(BackColor);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        if (_map != null)
        {
            var a = ToScreen(_mapRect.Left, _mapRect.Top);
            var b = ToScreen(_mapRect.Right, _mapRect.Bottom);
            g.InterpolationMode = _zoom * _mapRect.Width / _map.Width > 2 ? InterpolationMode.NearestNeighbor : InterpolationMode.HighQualityBilinear;
            g.DrawImage(_map, RectangleF.FromLTRB(a.X, a.Y, b.X, b.Y));
        }
        DrawGrid(g);
        if (_doc.Spec == null)
        {
            TextRenderer.DrawText(g, "Open a map, or import a model (File menu)", Font, ClientRectangle, Theme.Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            return;
        }
        var sel = _doc.Selected;
        foreach (var ar in _doc.Spec.Areas) DrawBox(g, ar, ar == sel ? Theme.Selected : Theme.Area, false, ar.Name);
        foreach (var w in _doc.Spec.Warps)
        {
            var (to, toYaw) = _doc.Destination(w);
            var a = ToScreen(w.X, w.Z); var b = ToScreen(to.X, to.Z);
            using (var p = new Pen(Color.FromArgb(160, Theme.WarpTo), 1.5f) { DashStyle = DashStyle.Dash, CustomEndCap = new AdjustableArrowCap(4, 5) })
                g.DrawLine(p, a, b);
            DrawPoint(g, to.X, to.Z, toYaw, w == sel ? Theme.Selected : Theme.WarpTo, 5, false);
            DrawBox(g, w, w == sel ? Theme.Selected : Theme.Warp, true, w.Name);
        }
        foreach (var s in _doc.Spec.Spawns) DrawPoint(g, s.X, s.Z, s.Yaw, s == sel ? Theme.Selected : Theme.Spawn, 7, true);
        foreach (var ev in _doc.Spec.EventPoints)
        {
            var c = ToScreen(ev.X, ev.Z);
            using var path = Theme.Star(c, 8);
            using var br = new SolidBrush(ev == sel ? Theme.Selected : Theme.Event);
            g.FillPath(br, path);
            if (_zoom > 0.05f) TextRenderer.DrawText(g, ev.Event.ToString(), Font, new Point((int)c.X + 9, (int)c.Y - 7), Color.FromArgb(200, Theme.Event));
        }
        if (sel is Box sb) DrawHandles(g, sb);
        else if (sel is Placed sp) DrawRotateHandle(g, sp.X, sp.Z, sp.Yaw, 26 / _zoom);
        // the scale
        TextRenderer.DrawText(g, $"grid {_doc.Grid:0}{(_doc.Snap ? " (snap)" : "")}   zoom {_zoom * 100:0.#}%", Font, new Point(8, Height - 20), Theme.Dim);
    }

    void DrawGrid(Graphics g)
    {
        float step = _doc.Grid > 0 ? _doc.Grid : 32;
        while (step * _zoom < 8) step *= 4;  // (never denser than 8 pixels)
        var tl = ToWorld(Point.Empty); var br = ToWorld(new Point(Width, Height));
        using var minor = new Pen(Color.FromArgb(_map != null ? 28 : 40, 255, 255, 255));
        using var major = new Pen(Color.FromArgb(_map != null ? 50 : 70, 255, 255, 255));
        using var axis = new Pen(Color.FromArgb(110, 0, 140, 255));
        int i0 = (int)MathF.Floor(tl.X / step), i1 = (int)MathF.Ceiling(br.X / step);
        for (int i = i0; i <= i1; i++)
        {
            float x = ToScreen(i * step, 0).X;
            g.DrawLine(i == 0 ? axis : i % 8 == 0 ? major : minor, x, 0, x, Height);
        }
        int j0 = (int)MathF.Floor(tl.Y / step), j1 = (int)MathF.Ceiling(br.Y / step);
        for (int j = j0; j <= j1; j++)
        {
            float y = ToScreen(0, j * step).Y;
            g.DrawLine(j == 0 ? axis : j % 8 == 0 ? major : minor, 0, y, Width, y);
        }
    }

    PointF[] Corners(Box b)
    {
        float hx = b.SizeX / 2, hz = b.SizeZ / 2;
        return new[] { Local(b, -hx, -hz), Local(b, hx, -hz), Local(b, hx, hz), Local(b, -hx, hz) }.Select(p => ToScreen(p.X, p.Y)).ToArray();
    }

    void DrawBox(Graphics g, Box b, Color c, bool fill, string name)
    {
        var pts = Corners(b);
        if (fill) using (var br = new SolidBrush(Color.FromArgb(60, c))) g.FillPolygon(br, pts);
        using (var p = new Pen(c, b == _doc.Selected ? 2f : 1.5f)) g.DrawPolygon(p, pts);
        // its facing: a tick from the middle to its front (-Z)
        var mid = ToScreen(b.X, b.Z); var front = Local(b, 0, -b.SizeZ / 2);
        using (var p = new Pen(c, 1.5f)) g.DrawLine(p, mid, ToScreen(front.X, front.Y));
        if (name != null && _zoom * Math.Max(b.SizeX, b.SizeZ) > 60)
            TextRenderer.DrawText(g, name, Font, new Point((int)pts[0].X + 4, (int)pts[0].Y + 2), c);
    }

    void DrawPoint(Graphics g, float x, float z, float yaw, Color c, float size, bool filled)
    {
        var p = ToScreen(x, z);
        float r = yaw * MathF.PI / 180f;
        var f = new PointF(p.X - MathF.Sin(r) * size * 1.8f, p.Y - MathF.Cos(r) * size * 1.8f);
        var l = new PointF(p.X - MathF.Cos(r) * size, p.Y + MathF.Sin(r) * size);
        var rr = new PointF(p.X + MathF.Cos(r) * size, p.Y - MathF.Sin(r) * size);
        var tri = new[] { f, l, rr };
        if (filled) using (var br = new SolidBrush(c)) g.FillPolygon(br, tri);
        using var pen = new Pen(c, 1.5f);
        g.DrawPolygon(pen, tri);
    }

    // handles: 4 corners and 4 edges (resize), one on a stick off the front (turn)
    List<(PointF at, int kind, float lx, float lz)> Handles(Box b)
    {
        float hx = b.SizeX / 2, hz = b.SizeZ / 2;
        var list = new List<(PointF, int, float, float)>();
        foreach (var (lx, lz) in new[] { (-1, -1), (1, -1), (1, 1), (-1, 1), (0, -1), (1, 0), (0, 1), (-1, 0) })
        {
            var w = Local(b, lx * hx, lz * hz);
            list.Add((ToScreen(w.X, w.Y), 1, lx, lz));
        }
        var rot = Local(b, 0, -hz - 30 / _zoom);
        list.Add((ToScreen(rot.X, rot.Y), 2, 0, 0));
        return list;
    }

    void DrawHandles(Graphics g, Box b)
    {
        foreach (var (at, kind, _, _) in Handles(b))
        {
            if (kind == 2)
            {
                var front = Local(b, 0, -b.SizeZ / 2);
                using (var p = new Pen(Theme.Selected)) g.DrawLine(p, ToScreen(front.X, front.Y), at);
                g.FillEllipse(Brushes.White, at.X - 5, at.Y - 5, 10, 10);
                continue;
            }
            g.FillRectangle(Brushes.White, at.X - 4, at.Y - 4, 8, 8);
            g.DrawRectangle(Pens.Black, at.X - 4, at.Y - 4, 8, 8);
        }
    }

    void DrawRotateHandle(Graphics g, float x, float z, float yaw, float dist)
    {
        var p = RotateHandleAt(x, z, yaw, dist);
        using (var pen = new Pen(Theme.Selected)) g.DrawLine(pen, ToScreen(x, z), p);
        g.FillEllipse(Brushes.White, p.X - 5, p.Y - 5, 10, 10);
    }
    PointF RotateHandleAt(float x, float z, float yaw, float dist)
    {
        float r = yaw * MathF.PI / 180f;
        return ToScreen(x - MathF.Sin(r) * dist, z - MathF.Cos(r) * dist);
    }

    // ---- picking ----
    object PickAt(Point s)
    {
        var w = ToWorld(s);
        float tol = 9 / _zoom;
        foreach (var ev in _doc.Spec.EventPoints) if (Dist(ev, w) < tol) return ev;
        foreach (var sp in _doc.Spec.Spawns) if (Dist(sp, w) < tol) return sp;
        foreach (var wp in _doc.Spec.Warps)
        {
            var (to, _) = _doc.Destination(wp);
            if (Inside(wp, w, tol)) return wp;
        }
        // areas: the smallest one the point is in
        return _doc.Spec.Areas.Where(a => Inside(a, w, tol)).OrderBy(a => a.SizeX * a.SizeZ).FirstOrDefault();
    }
    static float Dist(Placed p, PointF w) => MathF.Sqrt((p.X - w.X) * (p.X - w.X) + (p.Z - w.Y) * (p.Z - w.Y));
    static bool Inside(Box b, PointF w, float tol)
    {
        float r = b.Yaw * MathF.PI / 180f, c = MathF.Cos(r), s = MathF.Sin(r);
        float dx = w.X - b.X, dz = w.Y - b.Z;
        float lx = dx * c - dz * s, lz = dx * s + dz * c;
        return MathF.Abs(lx) <= b.SizeX / 2 + tol && MathF.Abs(lz) <= b.SizeZ / 2 + tol;
    }

    // ---- the mouse ----
    enum Drag { None, Pan, Move, Resize, Turn, ToolRotate, ToolScale }
    Drag _drag;
    Point _last, _down;
    PointF _grabOffset;
    float _heightAbove;
    (float lx, float lz) _handle;
    bool _checkpointed;

    protected override void OnMouseDown(MouseEventArgs e)
    {
        Focus();
        _last = _down = e.Location;
        _checkpointed = false;
        if (e.Button is MouseButtons.Right or MouseButtons.Middle) { _drag = Drag.Pan; Cursor = Cursors.SizeAll; return; }
        if (e.Button != MouseButtons.Left || _doc.Spec == null) return;
        var w = ToWorld(e.Location);
        // an object tool: place one
        if (_doc.Tool is Tool.Spawn or Tool.Area or Tool.Warp or Tool.Event)
        {
            float y = _doc.GroundBelow(new NVec3(w.X, 1e5f, w.Y), 0);
            _doc.Place(_doc.Tool, new NVec3(w.X, y, w.Y));
            _drag = Drag.Move;  // (and dragged on from there)
            BeginMove(_doc.Selected as Placed, w);
            return;
        }
        // the picked object's handles
        if (_doc.Selected is Box b && _doc.Tool is Tool.Select or Tool.Scale or Tool.Move)
            foreach (var (at, kind, lx, lz) in Handles(b))
                if (Math.Abs(at.X - e.X) <= 6 && Math.Abs(at.Y - e.Y) <= 6)
                {
                    _drag = kind == 2 ? Drag.Turn : Drag.Resize;
                    _handle = (lx, lz);
                    return;
                }
        if (_doc.Selected is Placed sp && sp is not Box)
        {
            var rh = RotateHandleAt(sp.X, sp.Z, sp.Yaw, 26 / _zoom);
            if (Math.Abs(rh.X - e.X) <= 6 && Math.Abs(rh.Y - e.Y) <= 6) { _drag = Drag.Turn; return; }
        }
        var hit = PickAt(e.Location);
        if (hit != null) _doc.Selected = hit;
        else if (_doc.Tool == Tool.Select) _doc.Selected = null;
        if (_doc.Selected is not Placed p) return;
        _drag = _doc.Tool switch { Tool.Rotate => Drag.ToolRotate, Tool.Scale => p is Box ? Drag.ToolScale : Drag.None, _ => hit == p ? Drag.Move : Drag.None };
        if (_drag == Drag.Move) BeginMove(p, w);
    }

    void BeginMove(Placed p, PointF w)
    {
        if (p == null) { _drag = Drag.None; return; }
        _grabOffset = new PointF(p.X - w.X, p.Z - w.Y);
        _heightAbove = p.Y - _doc.GroundBelow(new NVec3(p.X, p.Y + 50, p.Z), p.Y);
    }

    void Begin()
    {
        if (_checkpointed) return;
        _checkpointed = true;
        _doc.Checkpoint();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        var w = ToWorld(e.Location);
        CursorMoved?.Invoke(new NVec3(w.X, 0, w.Y));
        int dx = e.X - _last.X, dy = e.Y - _last.Y;
        _last = e.Location;
        switch (_drag)
        {
            case Drag.Pan:
                _centre = new PointF(_centre.X - dx / _zoom, _centre.Y - dy / _zoom);
                Invalidate();
                return;
            case Drag.Move when _doc.Selected is Placed p:
                if (Math.Abs(e.X - _down.X) + Math.Abs(e.Y - _down.Y) < 3 && !_checkpointed) return;  // (a click, not a drag)
                Begin();
                p.X = _doc.SnapTo(w.X + _grabOffset.X); p.Z = _doc.SnapTo(w.Y + _grabOffset.Y);
                p.Y = _doc.GroundBelow(new NVec3(p.X, 1e5f, p.Z), p.Y - _heightAbove) + _heightAbove;
                _doc.Touch();
                return;
            case Drag.Resize when _doc.Selected is Box b:
                Begin();
                {
                    // the opposite side stays where it is
                    float r = b.Yaw * MathF.PI / 180f, c = MathF.Cos(r), s = MathF.Sin(r);
                    float ddx = w.X - b.X, ddz = w.Y - b.Z;
                    float lx = ddx * c - ddz * s, lz = ddx * s + ddz * c;  // (the cursor, in the box's own frame)
                    float hx = b.SizeX / 2, hz = b.SizeZ / 2;
                    float minX = -hx, maxX = hx, minZ = -hz, maxZ = hz;
                    if (_handle.lx > 0) maxX = Math.Max(minX + 10, _doc.SnapTo(lx + b.X) - b.X);
                    if (_handle.lx < 0) minX = Math.Min(maxX - 10, _doc.SnapTo(lx + b.X) - b.X);
                    if (_handle.lz > 0) maxZ = Math.Max(minZ + 10, _doc.SnapTo(lz + b.Z) - b.Z);
                    if (_handle.lz < 0) minZ = Math.Min(maxZ - 10, _doc.SnapTo(lz + b.Z) - b.Z);
                    var mid = Local(b, (minX + maxX) / 2, (minZ + maxZ) / 2);
                    b.SizeX = maxX - minX; b.SizeZ = maxZ - minZ;
                    b.X = mid.X; b.Z = mid.Y;
                }
                _doc.Touch();
                return;
            case Drag.Turn or Drag.ToolRotate when _doc.Selected is Placed p:
                Begin();
                {
                    float yaw = MathF.Atan2(-(w.X - p.X), -(w.Y - p.Z)) * 180f / MathF.PI;
                    if (_doc.Snap || ModifierKeys.HasFlag(Keys.Shift)) yaw = MathF.Round(yaw / 15) * 15;
                    p.Yaw = (yaw % 360 + 360) % 360;
                }
                _doc.Touch();
                return;
            case Drag.ToolScale when _doc.Selected is Box b:
                Begin();
                {
                    float f = MathF.Max(0.2f, 1 - dy * 0.01f);
                    b.SizeX = MathF.Max(10, b.SizeX * f); b.SizeZ = MathF.Max(10, b.SizeZ * f); b.SizeY = MathF.Max(10, b.SizeY * f);
                }
                _doc.Touch();
                return;
        }
        // the cursor: over a handle, an object
        if (_doc.Spec == null) return;
        Cursor = _doc.Tool is Tool.Spawn or Tool.Area or Tool.Warp or Tool.Event ? Cursors.Cross : Cursors.Default;
        if (_doc.Selected is Box sb && _doc.Tool is Tool.Select or Tool.Move or Tool.Scale)
            foreach (var (at, kind, _, _) in Handles(sb))
                if (Math.Abs(at.X - e.X) <= 6 && Math.Abs(at.Y - e.Y) <= 6) { Cursor = kind == 2 ? Cursors.Hand : Cursors.SizeNWSE; break; }
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        _drag = Drag.None;
        Cursor = Cursors.Default;
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        var before = ToWorld(e.Location);
        _zoom *= e.Delta > 0 ? 1.2f : 1 / 1.2f;
        _zoom = Math.Clamp(_zoom, 0.002f, 20f);
        var after = ToWorld(e.Location);
        _centre = new PointF(_centre.X + before.X - after.X, _centre.Y + before.Y - after.Y);
        Invalidate();
    }

    protected override bool IsInputKey(Keys keyData) => keyData is Keys.Left or Keys.Right or Keys.Up or Keys.Down || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        // arrows: nudge by a grid step
        if (_doc.Selected is Placed p && e.KeyCode is Keys.Left or Keys.Right or Keys.Up or Keys.Down)
        {
            _doc.Checkpoint();
            float step = Math.Max(1, _doc.Grid);
            if (e.KeyCode == Keys.Left) p.X -= step; else if (e.KeyCode == Keys.Right) p.X += step;
            else if (e.KeyCode == Keys.Up) p.Z -= step; else p.Z += step;
            _doc.Touch();
            e.Handled = true;
        }
        base.OnKeyDown(e);
    }
}
