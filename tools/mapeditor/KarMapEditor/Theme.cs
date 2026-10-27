// The editor's look: dark, as level editors are (Hammer++, Trenchbroom); its icons from Windows' own icon font.
using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace KarMapEditor;

public static class Theme
{
    public static readonly Color Back = Color.FromArgb(37, 37, 40);
    public static readonly Color Surface = Color.FromArgb(45, 45, 49);
    public static readonly Color Raised = Color.FromArgb(58, 58, 63);
    public static readonly Color Border = Color.FromArgb(70, 70, 76);
    public static readonly Color Text = Color.FromArgb(225, 225, 228);
    public static readonly Color Dim = Color.FromArgb(150, 150, 158);
    public static readonly Color Accent = Color.FromArgb(0, 122, 204);
    public static readonly Color AccentDim = Color.FromArgb(38, 79, 120);
    public static readonly Color Area = Color.FromArgb(240, 200, 60);
    public static readonly Color Warp = Color.FromArgb(70, 180, 255);
    public static readonly Color WarpTo = Color.FromArgb(170, 130, 255);
    public static readonly Color Spawn = Color.FromArgb(90, 220, 110);
    public static readonly Color Event = Color.FromArgb(255, 130, 60);
    public static readonly Color Selected = Color.White;

    static readonly string IconFont = new InstalledFontCollection().Families.Any(f => f.Name == "Segoe Fluent Icons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";
    static readonly Dictionary<(char, int, int), Bitmap> s_icons = new();

    // An icon: a glyph of the icon font, in a colour (Text by default).
    public static Bitmap Icon(char glyph, int size = 20, Color? color = null)
    {
        var c = color ?? Text;
        var key = (glyph, size, c.ToArgb());
        if (s_icons.TryGetValue(key, out var bmp)) return bmp;
        bmp = new Bitmap(size, size);
        using (var g = Graphics.FromImage(bmp))
        {
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using var f = new Font(IconFont, size * 0.62f, GraphicsUnit.Pixel);
            using var b = new SolidBrush(c);
            var sz = g.MeasureString(glyph.ToString(), f, PointF.Empty, StringFormat.GenericTypographic);
            g.DrawString(glyph.ToString(), f, b, (size - sz.Width) / 2, (size - sz.Height) / 2, StringFormat.GenericTypographic);
        }
        return s_icons[key] = bmp;
    }

    // the glyphs used (Segoe Fluent Icons / MDL2 Assets)
    public const char GSelect = '', GMove = '', GRotate = '', GScale = '';
    public const char GSpawn = '', GArea = '', GWarp = '', GEvent = '';
    public const char GNew = '', GOpen = '', GSave = '', GImport = '', GUndo = '', GRedo = '';
    public const char GBuild = '', GPlay = '', GGrid = '', GFrame = '', GDelete = '', GCopy = '';
    public const char GMusic = '', GMap = '', GCloud = '', GSettings = '', GLayout = '', GStop = '';

    // dark colours for the menus and tool bars
    public class Renderer : ToolStripProfessionalRenderer
    {
        public Renderer() : base(new Colors()) { RoundedEdges = false; }
        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            e.TextColor = e.Item.Enabled ? Text : Dim;
            base.OnRenderItemText(e);
        }
        protected override void OnRenderArrow(ToolStripArrowRenderEventArgs e)
        {
            e.ArrowColor = Text;
            base.OnRenderArrow(e);
        }
        protected override void OnRenderButtonBackground(ToolStripItemRenderEventArgs e)
        {
            if (e.Item is ToolStripButton { Checked: true } b)
            {
                using var br = new SolidBrush(AccentDim);
                e.Graphics.FillRectangle(br, new Rectangle(Point.Empty, b.Size));
                using var p = new Pen(Accent);
                e.Graphics.DrawRectangle(p, new Rectangle(0, 0, b.Width - 1, b.Height - 1));
                return;
            }
            base.OnRenderButtonBackground(e);
        }
    }

    class Colors : ProfessionalColorTable
    {
        public override Color ToolStripGradientBegin => Surface;
        public override Color ToolStripGradientMiddle => Surface;
        public override Color ToolStripGradientEnd => Surface;
        public override Color ToolStripBorder => Surface;
        public override Color MenuStripGradientBegin => Back;
        public override Color MenuStripGradientEnd => Back;
        public override Color ToolStripDropDownBackground => Surface;
        public override Color ImageMarginGradientBegin => Surface;
        public override Color ImageMarginGradientMiddle => Surface;
        public override Color ImageMarginGradientEnd => Surface;
        public override Color MenuItemSelected => Raised;
        public override Color MenuItemSelectedGradientBegin => Raised;
        public override Color MenuItemSelectedGradientEnd => Raised;
        public override Color MenuItemPressedGradientBegin => Raised;
        public override Color MenuItemPressedGradientEnd => Raised;
        public override Color MenuItemBorder => Border;
        public override Color MenuBorder => Border;
        public override Color ButtonSelectedHighlight => Raised;
        public override Color ButtonSelectedGradientBegin => Raised;
        public override Color ButtonSelectedGradientEnd => Raised;
        public override Color ButtonPressedGradientBegin => AccentDim;
        public override Color ButtonPressedGradientEnd => AccentDim;
        public override Color ButtonCheckedGradientBegin => AccentDim;
        public override Color ButtonCheckedGradientEnd => AccentDim;
        public override Color ButtonSelectedBorder => Border;
        public override Color SeparatorDark => Border;
        public override Color SeparatorLight => Surface;
        public override Color StatusStripGradientBegin => Back;
        public override Color StatusStripGradientEnd => Back;
        public override Color ToolStripContentPanelGradientBegin => Back;
        public override Color ToolStripContentPanelGradientEnd => Back;
        public override Color CheckBackground => AccentDim;
        public override Color CheckSelectedBackground => Accent;
        public override Color CheckPressedBackground => Accent;
    }

    // a control tree in the dark colours (what WinForms has no theme for)
    public static void Apply(Control root)
    {
        foreach (Control c in root.Controls) Apply(c);
        switch (root)
        {
            case TextBox or ListBox or TreeView or ListView or NumericUpDown or ComboBox:
                root.BackColor = Back; root.ForeColor = Text;
                if (root is TextBox tb) tb.BorderStyle = BorderStyle.FixedSingle;
                if (root is TreeView tv) { tv.BorderStyle = BorderStyle.None; tv.LineColor = Dim; }
                if (root is ComboBox cb) cb.FlatStyle = FlatStyle.Flat;
                break;
            case PropertyGrid pg:
                pg.BackColor = Surface; pg.ViewBackColor = Back; pg.ViewForeColor = Text; pg.LineColor = Surface; pg.CategoryForeColor = Text;
                pg.CategorySplitterColor = Surface; pg.HelpBackColor = Surface; pg.HelpForeColor = Text; pg.ViewBorderColor = Border;
                pg.HelpBorderColor = Border; pg.CommandsBackColor = Surface; pg.CommandsForeColor = Text; pg.DisabledItemForeColor = Dim;
                pg.SelectedItemWithFocusBackColor = AccentDim; pg.SelectedItemWithFocusForeColor = Text;
                break;
            case Button b:
                b.FlatStyle = FlatStyle.Flat; b.BackColor = Raised; b.ForeColor = Text; b.FlatAppearance.BorderColor = Border;
                break;
            case CheckBox or Label or GroupBox or RadioButton:
                root.ForeColor = Text;
                break;
            case TabPage or Panel or SplitContainer or SplitterPanel or FlowLayoutPanel or TableLayoutPanel or UserControl:
                root.BackColor = Surface; root.ForeColor = Text;
                break;
        }
    }

    // a flat, dark tab strip
    public class Tabs : TabControl
    {
        public Tabs()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            SizeMode = TabSizeMode.Fixed;
            ItemSize = new Size(84, 26);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(Back);
            for (int i = 0; i < TabCount; i++)
            {
                var r = GetTabRect(i);
                bool sel = i == SelectedIndex;
                using (var b = new SolidBrush(sel ? Surface : Back)) e.Graphics.FillRectangle(b, r);
                if (sel) using (var p = new Pen(Accent, 2)) e.Graphics.DrawLine(p, r.Left, r.Top + 1, r.Right, r.Top + 1);
                TextRenderer.DrawText(e.Graphics, TabPages[i].Text, Font, r, sel ? Theme.Text : Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            }
            if (SelectedTab != null)
            {
                var pr = new Rectangle(0, ItemSize.Height + 2, Width, Height - ItemSize.Height - 2);
                using var b = new SolidBrush(Surface);
                e.Graphics.FillRectangle(b, pr);
            }
        }
    }

    public static GraphicsPath Star(PointF c, float r)
    {
        var p = new GraphicsPath();
        var pts = new PointF[10];
        for (int i = 0; i < 10; i++)
        {
            double a = -Math.PI / 2 + i * Math.PI / 5;
            float rr = i % 2 == 0 ? r : r * 0.45f;
            pts[i] = new PointF(c.X + (float)Math.Cos(a) * rr, c.Y + (float)Math.Sin(a) * rr);
        }
        p.AddPolygon(pts);
        return p;
    }
}
