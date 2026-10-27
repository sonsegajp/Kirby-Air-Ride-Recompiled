// The minimap builder: the map's minimaps as the HUD will show them - one for the whole map, or one per area that asks
// for its own (shown while the first rider is in it) - and how they are drawn: textured or one colour, outline,
// brightness, contrast, how see-through. The preview is drawn as the builder draws them.
using SixLabors.ImageSharp.PixelFormats;
using NVec3 = System.Numerics.Vector3;

namespace KarMapEditor;

public class MinimapPanel : UserControl
{
    readonly MapDoc _doc;
    readonly PropertyGrid _style = new() { Dock = DockStyle.Fill, ToolbarVisible = false, HelpVisible = false, PropertySort = PropertySort.NoSort };
    readonly CheckedListBox _areas = new() { Dock = DockStyle.Fill, CheckOnClick = true, IntegralHeight = false };
    readonly FlowLayoutPanel _pics = new() { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(6) };
    readonly Button _refresh = new() { Text = "  Draw the minimaps", Dock = DockStyle.Top, Height = 32, TextImageRelation = TextImageRelation.ImageBeforeText };
    bool _filling;
    int _version;

    public MinimapPanel(MapDoc doc)
    {
        _doc = doc;
        _refresh.Image = Theme.Icon(Theme.GMap, 18);
        var areasBox = new GroupBox { Text = "Areas with a minimap of their own", Dock = DockStyle.Fill, Padding = new Padding(6) };
        areasBox.Controls.Add(_areas);
        var styleBox = new GroupBox { Text = "Style", Dock = DockStyle.Fill, Padding = new Padding(6) };
        styleBox.Controls.Add(_style);
        var top = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal };
        top.Panel1.Controls.Add(areasBox);
        top.Panel2.Controls.Add(styleBox);
        var split = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal };
        split.Panel1.Controls.Add(top);
        split.Panel2.Controls.Add(_pics);
        split.Panel2.Controls.Add(_refresh);
        Controls.Add(split);
        Load += (_, _) => { split.SplitterDistance = Math.Max(120, Height * 45 / 100); top.SplitterDistance = Math.Max(60, top.Height / 2); };
        doc.StructureChanged += Fill;
        _areas.ItemCheck += (_, e) =>
        {
            if (_filling || _doc.Spec == null || e.Index >= _doc.Spec.Areas.Count) return;
            _doc.Checkpoint();
            _doc.Spec.Areas[e.Index].Minimap = e.NewValue == CheckState.Checked;
            _doc.Touch();
        };
        _style.PropertyValueChanged += (_, _) => _doc.Touch();
        _refresh.Click += (_, _) => Draw();
    }

    void Fill()
    {
        if (InvokeRequired) { BeginInvoke(Fill); return; }
        _filling = true;
        _areas.Items.Clear();
        if (_doc.Spec != null)
        {
            foreach (var a in _doc.Spec.Areas) _areas.Items.Add(a.Name ?? "area", a.Minimap);
            _style.SelectedObject = _doc.Spec.MinimapStyle ??= new MinimapStyle();
        }
        _filling = false;
    }

    async void Draw()
    {
        var scene = _doc.Scene;
        var spec = _doc.Spec;
        if (scene == null || spec == null || scene.Models.Count == 0) return;
        int version = ++_version;
        _refresh.Enabled = false;
        _refresh.Text = "  Drawing...";
        var regions = spec.Areas.Where(a => a.Minimap).Select(a =>
        {
            var c = a.Position; var h = new NVec3(a.SizeX, a.SizeY, a.SizeZ) * 0.5f;
            return (name: a.Name ?? "area", lo: c - h, hi: c + h);
        }).ToList();
        // (the model as it is in its file: the minimap is the same picture wherever the builder moves the map)
        var dir = Path.GetDirectoryName(spec.Model);
        var style = spec.MinimapStyle;
        List<(string name, Bitmap bmp)> pics = null;
        try
        {
            pics = await Task.Run(() => Minimap.Preview(scene, scene.Models[0], dir, regions, 1f, style).Select(p => (p.name, ToBitmap(p.image))).ToList());
        }
        catch (Exception e) { MessageBox.Show(this, e.Message, "Could not draw the minimaps"); }
        _refresh.Enabled = true;
        _refresh.Text = "  Draw the minimaps";
        if (pics == null || version != _version) return;
        foreach (Control c in _pics.Controls) c.Dispose();
        _pics.Controls.Clear();
        foreach (var (name, bmp) in pics)
        {
            var box = new Panel { Width = 2 * bmp.Width + 8, Height = 2 * bmp.Height + 30, Margin = new Padding(6) };
            var pb = new PictureBox { Image = bmp, SizeMode = PictureBoxSizeMode.Zoom, Dock = DockStyle.Fill, BackColor = Color.FromArgb(20, 40, 60) };
            box.Controls.Add(pb);
            box.Controls.Add(new Label { Text = name, Dock = DockStyle.Top, Height = 22, ForeColor = Theme.Text, TextAlign = ContentAlignment.MiddleCenter });
            _pics.Controls.Add(box);
        }
    }

    public static Bitmap ToBitmap(SixLabors.ImageSharp.Image<Rgba32> img)
    {
        var bmp = new Bitmap(img.Width, img.Height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        var data = bmp.LockBits(new Rectangle(0, 0, img.Width, img.Height), System.Drawing.Imaging.ImageLockMode.WriteOnly, bmp.PixelFormat);
        var px = new byte[img.Width * img.Height * 4];
        img.CloneAs<Bgra32>().CopyPixelDataTo(px);
        System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
        bmp.UnlockBits(data);
        img.Dispose();
        return bmp;
    }
}
