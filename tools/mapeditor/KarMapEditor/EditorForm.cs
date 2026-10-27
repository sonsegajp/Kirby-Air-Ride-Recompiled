// The editor's window, laid out as level editors are (Hammer): tools down the left, the 3D view and the view from above
// in the middle, the objects, the map's settings, its music and its minimaps on the right, the builder's messages below.
//   File: a new map from a model (any of .gltf .glb .obj .fbx .dae .smd, or a scene of Ocarina of Time from Ship of
//         Harkinian), open, save. Map: build it as a mod (its collision, minimaps and music made then), play it.
//   Tools: Select / Move / Rotate / Scale (Q W E R), then what can be placed: Spawn, Area, Warp, Event point (1-4).
using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;
using HSDRaw;
using HSDRawViewer.GUI;
using IONET.Core;
using OpenTK.Mathematics;
using NVec3 = System.Numerics.Vector3;

namespace KarMapEditor;

public class EditorForm : Form
{
    readonly MapDoc _doc = new();
    readonly ViewportControl _vp;
    readonly MapCamera _cam = new();
    readonly MapView _view;
    readonly TopView _top;
    readonly TreeView _tree = new() { Dock = DockStyle.Fill, HideSelection = false, FullRowSelect = true, ShowLines = false, ItemHeight = 22 };
    readonly PropertyGrid _props = new() { Dock = DockStyle.Fill, ToolbarVisible = false, HelpVisible = true };
    readonly PropertyGrid _mapProps = new() { Dock = DockStyle.Fill, ToolbarVisible = false, HelpVisible = true };
    readonly TextBox _log = new() { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9) };
    readonly ToolStripStatusLabel _status = new() { Spring = true, TextAlign = ContentAlignment.MiddleLeft };
    readonly ToolStripStatusLabel _coords = new() { Width = 220, TextAlign = ContentAlignment.MiddleRight, AutoSize = false };
    readonly ToolStripProgressBar _progress = new() { Visible = false, Style = ProgressBarStyle.Marquee, Width = 140 };
    readonly Dictionary<Tool, ToolStripButton> _toolButtons = new();
    readonly ToolStripComboBox _eventKind = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 130, ToolTipText = "The event an event point is for" };
    readonly ToolStripComboBox _gridSize = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 60, ToolTipText = "Grid size ([ and ])" };
    readonly ToolStripButton _snap = new() { CheckOnClick = true, Checked = true, ToolTipText = "Snap to the grid (G)" };
    readonly SplitContainer _views = new() { Dock = DockStyle.Fill };
    readonly ImageList _icons = new() { ImageSize = new Size(16, 16), ColorDepth = ColorDepth.Depth32Bit };
    readonly Settings _settings = Settings.Load();
    bool _building, _syncing;
    string _snapshot;  // (the spec before a property is changed: what undo goes back to)

    static readonly float[] Grids = { 1, 4, 8, 16, 32, 64, 128, 256, 512 };

    public EditorForm(string open)
    {
        Text = "Kirby Air Ride Map Editor";
        Width = 1700; Height = 1000;
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        BackColor = Theme.Back; ForeColor = Theme.Text;
        Icon = System.Drawing.Icon.FromHandle(Theme.Icon(Theme.GMap, 32, Theme.Accent).GetHicon());

        _view = new MapView(_doc);
        _vp = new ViewportControl { Dock = DockStyle.Fill };
        _cam.FarClipPlane = 200000;
        _cam.NearClipPlane = 5;
        _cam.FovDegrees = 50;
        _vp.EnableHelpDisplay = false;
        _vp.AnimationTrackEnabled = false;
        _vp.AddRenderer(_view);
        // (the viewer's own tool bar - screenshots, its camera - not this editor's)
        foreach (Control c in _vp.Controls) if (c is ToolStrip ts) ts.Visible = false;
        _vp.ViewportBackColor = Color.FromArgb(28, 28, 30);
        _top = new TopView(_doc) { Dock = DockStyle.Fill };

        foreach (var (key, g, c) in new[] { ("map", Theme.GMap, Theme.Accent), ("spawn", Theme.GSpawn, Theme.Spawn), ("area", Theme.GArea, Theme.Area),
                                            ("warp", Theme.GWarp, Theme.Warp), ("event", Theme.GEvent, Theme.Event), ("group", Theme.GLayout, Theme.Dim) })
            _icons.Images.Add(key, Theme.Icon(g, 16, c));
        _tree.ImageList = _icons;

        var renderer = new Theme.Renderer();
        var menu = BuildMenu();
        menu.Renderer = renderer;
        var bar = BuildToolbar();
        bar.Renderer = renderer;
        var tools = BuildToolbox();
        tools.Renderer = renderer;
        var statusBar = new StatusStrip { Renderer = renderer, BackColor = Theme.Back };
        statusBar.Items.AddRange(new ToolStripItem[] { _status, _progress, _coords });

        // ---- the middle: 3D | from above ----
        var label3d = ViewLabel("3D   (left-drag: turn  right-drag: slide  wheel: zoom  double-click: look there)");
        var labelTop = ViewLabel("Top   (right-drag: slide  wheel: zoom  drag: move  handles: size, turn)");
        _views.Panel1.Controls.Add(_vp);
        _views.Panel1.Controls.Add(label3d);
        _views.Panel2.Controls.Add(_top);
        _views.Panel2.Controls.Add(labelTop);
        var middle = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, FixedPanel = FixedPanel.Panel2 };
        middle.Panel1.Controls.Add(_views);
        middle.Panel2.Controls.Add(_log);

        // ---- the right: tabs ----
        var tabs = new Theme.Tabs { Dock = DockStyle.Fill };
        var objects = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal };
        objects.Panel1.Controls.Add(_tree);
        objects.Panel2.Controls.Add(_props);
        tabs.TabPages.Add(new TabPage("Objects") { Controls = { objects } });
        tabs.TabPages.Add(new TabPage("Map") { Controls = { _mapProps } });
        tabs.TabPages.Add(new TabPage("Music") { Controls = { new MusicPanel(_doc) { Dock = DockStyle.Fill } } });
        tabs.TabPages.Add(new TabPage("Minimap") { Controls = { new MinimapPanel(_doc) { Dock = DockStyle.Fill } } });

        var main = new SplitContainer { Dock = DockStyle.Fill, FixedPanel = FixedPanel.Panel2 };
        main.Panel1.Controls.Add(middle);
        main.Panel2.Controls.Add(tabs);
        Controls.Add(main);
        Controls.Add(tools);
        Controls.Add(bar);
        Controls.Add(menu);
        Controls.Add(statusBar);
        MainMenuStrip = menu;
        Theme.Apply(this);
        _log.BackColor = Color.FromArgb(24, 24, 26); _log.ForeColor = Theme.Dim;
        Load += (_, _) =>
        {
            main.SplitterDistance = Math.Max(400, main.Width - 470);
            middle.SplitterDistance = Math.Max(200, middle.Height - 120);
            _views.SplitterDistance = _views.Width / 2;
            objects.SplitterDistance = (int)(objects.Height * 0.45);
        };

        // ---- wiring ----
        _doc.SelectionChanged += () => { if (!_syncing) SyncSelection(); };
        _doc.StructureChanged += RefreshTree;
        _doc.Changed += () => { _props.Refresh(); UpdateTitle(); };
        _doc.ToolChanged += () => { foreach (var (t, b) in _toolButtons) b.Checked = t == _doc.Tool; _status.Text = ToolHint(); };
        _tree.AfterSelect += (_, e) =>
        {
            if (_syncing) return;
            _syncing = true;
            _doc.Selected = e.Node?.Tag is Placed p ? p : null;
            _props.SelectedObject = e.Node?.Tag is MapSpec ? new MapSettings(_doc.Spec) : e.Node?.Tag as Placed;
            _syncing = false;
        };
        _tree.NodeMouseDoubleClick += (_, e) => { if (e.Node?.Tag is Placed p) { _top.FrameOn(p); LookAt(p); } };
        foreach (var pg in new[] { _props, _mapProps })
        {
            pg.SelectedGridItemChanged += (_, _) => _snapshot = Snapshot();
            pg.PropertyValueChanged += (_, e) =>
            {
                _doc.CheckpointFrom(_snapshot);
                _snapshot = Snapshot();
                if (e.ChangedItem?.Label is "Model" or "Z is up" or "Scale") ReloadModel();
                if (e.ChangedItem?.Label is "Name" or "To" or "Event") RefreshTree();
                _doc.Touch();
            };
        }
        _view.GroundDoubleClicked += g => BeginInvoke(() => _cam.Recentre(new Vector3(g.X, g.Y, g.Z)));
        _view.KeyPressed += k => BeginInvoke(() => ViewKey(k));
        _top.KeyDown += (_, k) => ViewKey(k);
        _top.CursorMoved += p => _coords.Text = $"X {p.X:0}   Z {p.Z:0}";
        KeyDown += (_, e) => { if (e.KeyCode == Keys.Home && !(ActiveControl is TextBoxBase)) { FrameAll(); e.Handled = true; } };
        FormClosing += (_, e) => { if (!ConfirmDiscard()) e.Cancel = true; };
        Shown += (_, _) => { _vp.Camera = _cam; FrameAll(); };  // (the view makes a camera of its own as it loads: ours after)

        var start = open ?? (_settings.LastMap != null && File.Exists(_settings.LastMap) ? _settings.LastMap : null);
        if (start != null) Shown += (_, _) => Open(start);
        else UpdateTitle();
        _status.Text = ToolHint();
        // (KARED_SHOT=<file.png>: the window saved there a few seconds after opening - checking the editor draws)
        if (Environment.GetEnvironmentVariable("KARED_SHOT") is string shot)
        {
            var t = new System.Windows.Forms.Timer { Interval = 6000 };
            t.Tick += (_, _) =>
            {
                t.Stop();
                using var gl = _vp.GenerateBitmap(_vp.Width, Math.Max(100, _vp.Height));
                SixLabors.ImageSharp.ImageExtensions.SaveAsPng(gl, Path.ChangeExtension(shot, ".3d.png"));
                using var bmp = new Bitmap(Width, Height);
                DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
                bmp.Save(shot);
            };
            Shown += (_, _) => t.Start();
        }
    }

    // a tool bar's drop-down list, drawn dark (Windows draws a list's box itself, white)
    static void DarkCombo(ToolStripComboBox c)
    {
        var cb = c.ComboBox;
        cb.DrawMode = DrawMode.OwnerDrawFixed;
        cb.FlatStyle = FlatStyle.Flat;
        cb.BackColor = Theme.Back; cb.ForeColor = Theme.Text;
        cb.DrawItem += (_, e) =>
        {
            bool sel = (e.State & DrawItemState.Selected) != 0 && (e.State & DrawItemState.ComboBoxEdit) == 0;
            using (var b = new SolidBrush(sel ? Theme.AccentDim : Theme.Back)) e.Graphics.FillRectangle(b, e.Bounds);
            if (e.Index >= 0) TextRenderer.DrawText(e.Graphics, cb.Items[e.Index].ToString(), cb.Font, e.Bounds, Theme.Text, TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
        };
    }

    static Label ViewLabel(string text) => new() { Text = text, Dock = DockStyle.Top, Height = 20, ForeColor = Theme.Dim, BackColor = Theme.Back, Padding = new Padding(6, 3, 0, 0) };

    // ---- menus and tool bars ----
    MenuStrip BuildMenu()
    {
        var menu = new MenuStrip { BackColor = Theme.Back, ForeColor = Theme.Text };
        ToolStripMenuItem Item(string text, char glyph, Action act, Keys keys = Keys.None, string keyText = null)
        {
            var it = new ToolStripMenuItem(text, glyph == '\0' ? null : Theme.Icon(glyph, 16), (_, _) => act()) { ForeColor = Theme.Text };
            if (keys != Keys.None) it.ShortcutKeys = keys;
            if (keyText != null) it.ShortcutKeyDisplayString = keyText;
            return it;
        }
        var file = new ToolStripMenuItem("&File") { ForeColor = Theme.Text };
        file.DropDownItems.AddRange(new ToolStripItem[]
        {
            Item("&New map from a model...", Theme.GNew, NewFromModel, Keys.Control | Keys.N),
            Item("New map from &Ocarina of Time (Ship of Harkinian)...", Theme.GImport, ImportSoh),
            Item("&Open map...", Theme.GOpen, OpenDialog, Keys.Control | Keys.O),
            new ToolStripSeparator(),
            Item("&Save", Theme.GSave, () => Save(), Keys.Control | Keys.S),
            Item("Save &as...", '\0', () => SaveAs()),
            new ToolStripSeparator(),
            Item("E&xit", '\0', Close),
        });
        var edit = new ToolStripMenuItem("&Edit") { ForeColor = Theme.Text };
        edit.DropDownItems.AddRange(new ToolStripItem[]
        {
            Item("&Undo", Theme.GUndo, _doc.Undo, Keys.Control | Keys.Z),
            Item("&Redo", Theme.GRedo, _doc.Redo, Keys.Control | Keys.Y),
            new ToolStripSeparator(),
            Item("&Copy here", Theme.GCopy, _doc.DuplicateSelected, Keys.Control | Keys.D),
            Item("&Delete", Theme.GDelete, _doc.DeleteSelected, Keys.None, "Del"),
        });
        var view = new ToolStripMenuItem("&View") { ForeColor = Theme.Text };
        ToolStripMenuItem Toggle(string name, Func<bool> get, Action<bool> set)
        {
            var it = new ToolStripMenuItem(name) { Checked = get(), CheckOnClick = true, ForeColor = Theme.Text };
            it.CheckedChanged += (_, _) => { set(it.Checked); _top.Invalidate(); };
            return it;
        }
        view.DropDownItems.AddRange(new ToolStripItem[]
        {
            Item("3D and top", Theme.GLayout, () => Layout(0)), Item("3D only", '\0', () => Layout(1)), Item("Top only", '\0', () => Layout(2)),
            new ToolStripSeparator(),
            Toggle("Areas", () => _view.ShowAreas, v => _view.ShowAreas = v), Toggle("Warps", () => _view.ShowWarps, v => _view.ShowWarps = v),
            Toggle("Spawns", () => _view.ShowSpawns, v => _view.ShowSpawns = v), Toggle("Event points", () => _view.ShowEvents, v => _view.ShowEvents = v),
            new ToolStripSeparator(),
            Item("&Whole map", Theme.GFrame, FrameAll, Keys.None, "Home"),
            Item("&Look at the picked one", '\0', () => { if (_doc.Selected is Placed p) { _top.FrameOn(p); LookAt(p); } }, Keys.None, "F"),
        });
        var map = new ToolStripMenuItem("&Map") { ForeColor = Theme.Text };
        map.DropDownItems.AddRange(new ToolStripItem[]
        {
            Item("&Build the mod", Theme.GBuild, () => Build(false), Keys.F5),
            Item("Build and &play", Theme.GPlay, () => Build(true), Keys.F6),
            Item("Open the mod's &folder", Theme.GOpen, () => { if (_doc.Path != null) { Directory.CreateDirectory(ModDir); Process.Start("explorer.exe", ModDir); } }),
            new ToolStripSeparator(),
            Item("The game's &files (GrCity1.dat...)...", Theme.GSettings, () => AskCityDir()),
            Item("The &game (kirbyairride.exe)...", Theme.GSettings, () => AskGame()),
        });
        menu.Items.AddRange(new ToolStripItem[] { file, edit, view, map });
        return menu;
    }

    ToolStrip BuildToolbar()
    {
        var bar = new ToolStrip { GripStyle = ToolStripGripStyle.Hidden, ImageScalingSize = new Size(20, 20), Padding = new Padding(4, 2, 4, 2), BackColor = Theme.Surface };
        ToolStripButton B(char g, string tip, Action act)
        {
            var b = new ToolStripButton(Theme.Icon(g)) { ToolTipText = tip, DisplayStyle = ToolStripItemDisplayStyle.Image, AutoSize = false, Size = new Size(32, 30) };
            b.Click += (_, _) => act();
            return b;
        }
        foreach (var g in Grids) _gridSize.Items.Add(g.ToString());
        _gridSize.SelectedItem = "32";
        _gridSize.SelectedIndexChanged += (_, _) => { _doc.Grid = float.Parse(_gridSize.SelectedItem.ToString()); _top.Invalidate(); };
        DarkCombo(_gridSize);
        _snap.Image = Theme.Icon(Theme.GGrid);
        _snap.CheckedChanged += (_, _) => { _doc.Snap = _snap.Checked; _top.Invalidate(); };
        var build = new ToolStripButton("Build", Theme.Icon(Theme.GBuild)) { ToolTipText = "Build the map as a mod (F5)", ForeColor = Theme.Text };
        build.Click += (_, _) => Build(false);
        var play = new ToolStripButton("Play", Theme.Icon(Theme.GPlay, 20, Theme.Spawn)) { ToolTipText = "Build it and play it in the game (F6)", ForeColor = Theme.Text };
        play.Click += (_, _) => Build(true);
        bar.Items.AddRange(new ToolStripItem[]
        {
            B(Theme.GNew, "New map from a model (Ctrl+N)", NewFromModel), B(Theme.GOpen, "Open a map (Ctrl+O)", OpenDialog), B(Theme.GSave, "Save (Ctrl+S)", () => Save()),
            new ToolStripSeparator(),
            B(Theme.GUndo, "Undo (Ctrl+Z)", _doc.Undo), B(Theme.GRedo, "Redo (Ctrl+Y)", _doc.Redo),
            new ToolStripSeparator(),
            B(Theme.GCopy, "Copy the picked one (Ctrl+D)", _doc.DuplicateSelected), B(Theme.GDelete, "Delete the picked one (Del)", _doc.DeleteSelected),
            new ToolStripSeparator(),
            new ToolStripLabel("Grid") { ForeColor = Theme.Dim }, _gridSize, _snap,
            new ToolStripSeparator(),
            B(Theme.GFrame, "Whole map (Home)", FrameAll), B(Theme.GLayout, "3D and top / 3D / top", () => Layout(-1)),
            new ToolStripSeparator(),
            build, play,
        });
        return bar;
    }

    ToolStrip BuildToolbox()
    {
        var box = new ToolStrip { Dock = DockStyle.Left, GripStyle = ToolStripGripStyle.Hidden, ImageScalingSize = new Size(22, 22), LayoutStyle = ToolStripLayoutStyle.VerticalStackWithOverflow, BackColor = Theme.Surface, Padding = new Padding(3) };
        void T(Tool t, char g, string tip, Color? c = null)
        {
            var b = new ToolStripButton(Theme.Icon(g, 22, c)) { ToolTipText = tip, DisplayStyle = ToolStripItemDisplayStyle.Image, AutoSize = false, Size = new Size(38, 36), Checked = t == _doc.Tool };
            b.Click += (_, _) => _doc.Tool = t;
            _toolButtons[t] = b;
            box.Items.Add(b);
        }
        T(Tool.Select, Theme.GSelect, "Select (Q): click to pick, drag to move; a box's handles size and turn it");
        T(Tool.Move, Theme.GMove, "Move (W): drag the picked one over the ground");
        T(Tool.Rotate, Theme.GRotate, "Rotate (E): drag to turn the picked one");
        T(Tool.Scale, Theme.GScale, "Scale (R): drag to size the picked box");
        box.Items.Add(new ToolStripSeparator());
        T(Tool.Spawn, Theme.GSpawn, "Spawn (1): click the map to place where a rider starts", Theme.Spawn);
        T(Tool.Area, Theme.GArea, "Area (2): a part of the map with its own music and minimap", Theme.Area);
        T(Tool.Warp, Theme.GWarp, "Warp (3): ride into it to be put somewhere else", Theme.Warp);
        T(Tool.Event, Theme.GEvent, "Event point (4): where a City Trial event happens (pick the event below)", Theme.Event);
        foreach (var k in Enum.GetValues<EventKind>()) _eventKind.Items.Add(k);
        _eventKind.SelectedItem = EventKind.DynaBlade;
        _eventKind.SelectedIndexChanged += (_, _) => { _doc.PlaceEvent = (EventKind)_eventKind.SelectedItem; _doc.Tool = Tool.Event; };
        DarkCombo(_eventKind);
        _eventKind.Width = 120;
        box.Items.Add(new ToolStripSeparator());
        box.Items.Add(_eventKind);
        return box;
    }

    string ToolHint() => _doc.Tool switch
    {
        Tool.Select => "Select: click to pick; drag it to move; square handles size it, the round one turns it. Del deletes, Ctrl+D copies.",
        Tool.Move => "Move: drag the picked one over the ground (on the grid when snapping). Arrow keys nudge it.",
        Tool.Rotate => "Rotate: drag to turn the picked one (15 degree steps when snapping).",
        Tool.Scale => "Scale: drag up and down to size the picked box.",
        Tool.Spawn => "Spawn: click the map to place one. Riders start at the spawns.",
        Tool.Area => "Area: click the map to place one; give it music (Music tab) and its own minimap (Minimap tab).",
        Tool.Warp => "Warp: click the map to place one; set where it leads (To, or ToX/ToY/ToZ) in its properties.",
        Tool.Event => $"Event point ({_doc.PlaceEvent}): click the map where the event should happen. Events with none get points of their own.",
        _ => "",
    };

    int _layout;
    void Layout(int mode)
    {
        _layout = mode < 0 ? (_layout + 1) % 3 : mode;
        _views.Panel1Collapsed = _layout == 2;
        _views.Panel2Collapsed = _layout == 1;
    }

    // ---- maps ----
    void OpenDialog()
    {
        if (!ConfirmDiscard()) return;
        using var d = new OpenFileDialog { Filter = "City Trial map (map.json)|*.json|All files|*.*", Title = "Open a map" };
        if (d.ShowDialog(this) == DialogResult.OK) Open(d.FileName);
    }

    void Open(string path)
    {
        try
        {
            var spec = MapSpec.Load(path);
            spec.Fog ??= new FogSettings();
            spec.MinimapStyle ??= new MinimapStyle();
            _doc.Load(spec, Path.GetFullPath(path), null);
            _settings.LastMap = _doc.Path; _settings.Save();
            Log($"opened {_doc.Path}");
            ReloadModel();
            _mapProps.SelectedObject = new MapSettings(spec);
            _doc.Selected = null;
        }
        catch (Exception e) { MessageBox.Show(this, e.Message, "Could not open the map", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        UpdateTitle();
    }

    void NewFromModel()
    {
        if (!ConfirmDiscard()) return;
        using var d = new OpenFileDialog { Filter = "Models|*.gltf;*.glb;*.obj;*.fbx;*.dae;*.smd|All files|*.*", Title = "The map's model" };
        if (d.ShowDialog(this) != DialogResult.OK) return;
        var spec = new MapSpec { Name = Path.GetFileNameWithoutExtension(d.FileName), Model = d.FileName };
        var path = Path.Combine(Path.GetDirectoryName(d.FileName), "map.json");
        _doc.Load(spec, File.Exists(path) ? null : path, null);  // (an existing map.json is not written over unasked)
        _doc.Modified = true;
        ReloadModel();
        // four spawns in the middle, on the ground
        var mid = (_view.Min + _view.Max) / 2;
        float y = _view.GroundBelow(mid, mid.Y);
        for (int i = 0; i < 4; i++) spec.Spawns.Add(new Placed { X = mid.X + (i - 1.5f) * 15, Y = y + 10, Z = mid.Z });
        _mapProps.SelectedObject = new MapSettings(spec);
        _doc.TouchStructure();
        UpdateTitle();
    }

    // a scene of Ocarina of Time (from Ship of Harkinian's oot.o2r) as a map, with the places its exits lead to
    void ImportSoh()
    {
        if (!ConfirmDiscard()) return;
        using var f = new Form { Text = "Ocarina of Time scene", Width = 520, Height = 230, FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, MaximizeBox = false, MinimizeBox = false };
        var o2r = new TextBox { Left = 120, Top = 16, Width = 300, Text = _settings.SohArchive ?? "" };
        var browse = new Button { Left = 426, Top = 14, Width = 70, Text = "Browse" };
        var scene = new ComboBox { Left = 120, Top = 52, Width = 200, DropDownStyle = ComboBoxStyle.DropDown, Text = "spot00" };
        scene.Items.AddRange(new object[] { "spot00 (Hyrule Field)", "spot01 (Kakariko)", "spot04 (Kokiri Forest)", "spot09 (Gerudo Valley)", "spot15 (Hyrule Castle)", "spot20 (Lon Lon Ranch)" });
        var alone = new CheckBox { Left = 120, Top = 88, Width = 360, Text = "This scene alone (else with the places its exits lead to)" };
        var ok = new Button { Left = 316, Top = 140, Width = 86, Text = "Import", DialogResult = DialogResult.OK };
        var cancel = new Button { Left = 410, Top = 140, Width = 86, Text = "Cancel", DialogResult = DialogResult.Cancel };
        f.Controls.AddRange(new Control[] { new Label { Left = 12, Top = 19, Width = 100, Text = "oot.o2r" }, o2r, browse, new Label { Left = 12, Top = 55, Width = 100, Text = "Scene" }, scene, alone, ok, cancel });
        browse.Click += (_, _) => { using var d = new OpenFileDialog { Filter = "Ship of Harkinian archive|*.o2r|All files|*.*" }; if (d.ShowDialog(f) == DialogResult.OK) o2r.Text = d.FileName; };
        f.AcceptButton = ok; f.CancelButton = cancel;
        f.BackColor = Theme.Surface; Theme.Apply(f);
        if (f.ShowDialog(this) != DialogResult.OK || !File.Exists(o2r.Text)) return;
        _settings.SohArchive = o2r.Text; _settings.Save();
        var name = scene.Text.Split(' ')[0];
        using var save = new FolderBrowserDialog { Description = "Where the map goes (a folder of its own)", UseDescriptionForTitle = true };
        if (save.ShowDialog(this) != DialogResult.OK) return;
        var outDir = save.SelectedPath;
        RunBusy($"importing {name}", () =>
        {
            if (alone.Checked) SohScene.Import(o2r.Text, name, outDir, 0.25f, s => BeginInvoke(() => Log(s)));
            else SohWorld.Import(o2r.Text, name, outDir, 0.25f, s => BeginInvoke(() => Log(s)));
        }, () => { var mj = Path.Combine(outDir, "map.json"); if (File.Exists(mj)) Open(mj); });
    }

    void ReloadModel()
    {
        IOScene scene = null;
        if (_doc.Spec?.Model != null)
        {
            try
            {
                Cursor = Cursors.WaitCursor;
                scene = GltfIO.Load(_doc.Spec.Model);
                int tris = scene.Models.Count > 0 ? scene.Models[0].Meshes.Sum(m => m.Polygons.Sum(p => p.Indicies.Count / 3)) : 0;
                Log($"model {Path.GetFileName(_doc.Spec.Model)}: {tris} triangles, {scene.Materials.Count} materials");
            }
            catch (Exception e) { Log($"could not load the model: {e.Message}"); }
            finally { Cursor = Cursors.Default; }
        }
        _doc.Scene = scene;
        _view.Load(scene);
        FrameAll();
        DrawTopMap();
    }

    // the view from above's picture of the map (drawn as the minimap is, bigger)
    async void DrawTopMap()
    {
        var scene = _doc.Scene;
        if (scene == null || scene.Models.Count == 0) return;
        var lo = _view.Min; var hi = _view.Max;
        float w = hi.X - lo.X, h = hi.Z - lo.Z;
        if (w <= 0 || h <= 0) return;
        int px = (int)Math.Min(2048, Math.Max(256, 2048 * w / Math.Max(w, h))), pz = (int)Math.Min(2048, Math.Max(256, 2048 * h / Math.Max(w, h)));
        var dir = Path.GetDirectoryName(_doc.Spec.Model);
        try
        {
            var img = await Task.Run(() => Minimap.Render(scene.Models[0], Minimap.Textures(scene, dir), new System.Numerics.Vector2((lo.X + hi.X) / 2, (lo.Z + hi.Z) / 2),
                                                          w / 2, h / 2, false, lo, hi, px, pz, new MinimapStyle(), false, 1));
            if (scene == _doc.Scene) _top.SetMap(img, RectangleF.FromLTRB(lo.X, lo.Z, hi.X, hi.Z));
            _top.FrameAll();
        }
        catch (Exception e) { Log($"(the view from above: {e.Message})"); }
    }

    static readonly JsonSerializerOptions JsonOut = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    string Snapshot() => _doc.Spec == null ? null : JsonSerializer.Serialize(_doc.Spec, JsonOut);

    bool Save()
    {
        if (_doc.Spec == null) return false;
        if (_doc.Path == null) return SaveAs();
        // (written with paths beside the map's file as relative ones, as they were)
        var copy = JsonSerializer.Deserialize<MapSpec>(JsonSerializer.Serialize(_doc.Spec, JsonOut), new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        var dir = Path.GetDirectoryName(_doc.Path);
        string Rel(string p)
        {
            if (string.IsNullOrEmpty(p) || !Path.IsPathRooted(p)) return p;
            var r = Path.GetRelativePath(dir, p);
            return r.StartsWith("..") || Path.IsPathRooted(r) ? p : r.Replace('\\', '/');
        }
        copy.Model = Rel(copy.Model); copy.Collision = Rel(copy.Collision);
        copy.SkyPanorama = Rel(copy.SkyPanorama); copy.SkyTop = Rel(copy.SkyTop); copy.Music = Rel(copy.Music);
        foreach (var a in copy.Areas) a.Music = Rel(a.Music);
        File.WriteAllText(_doc.Path, JsonSerializer.Serialize(copy, JsonOut));
        _doc.Modified = false;
        _settings.LastMap = _doc.Path; _settings.Save();
        Log($"saved {_doc.Path}");
        UpdateTitle();
        return true;
    }

    bool SaveAs()
    {
        if (_doc.Spec == null) return false;
        using var d = new SaveFileDialog { Filter = "City Trial map (map.json)|*.json", FileName = "map.json", InitialDirectory = _doc.Spec.Model != null ? Path.GetDirectoryName(_doc.Spec.Model) : null };
        if (d.ShowDialog(this) != DialogResult.OK) return false;
        _doc.Path = d.FileName;
        return Save();
    }

    bool ConfirmDiscard()
    {
        if (!_doc.Modified || _doc.Spec == null) return true;
        var r = MessageBox.Show(this, "Save the map's changes first?", Text, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        return r == DialogResult.No || (r == DialogResult.Yes && Save());
    }

    void UpdateTitle() => Text = (_doc.Spec == null ? "" : $"{_doc.Spec.Name}{(_doc.Modified ? " *" : "")} - ") + "Kirby Air Ride Map Editor";

    // ---- the object list ----
    void RefreshTree()
    {
        if (InvokeRequired) { BeginInvoke(RefreshTree); return; }
        _syncing = true;
        _tree.BeginUpdate();
        _tree.Nodes.Clear();
        var spec = _doc.Spec;
        if (spec != null)
        {
            _tree.Nodes.Add(new TreeNode(spec.Name ?? "Map", 0, 0) { Tag = spec });
            void Group(string name, string icon, IEnumerable<object> items, Func<object, int, string> label)
            {
                var g = new TreeNode(name) { ImageKey = icon, SelectedImageKey = icon };
                int i = 0;
                foreach (var o in items) g.Nodes.Add(new TreeNode(label(o, i++)) { Tag = o, ImageKey = icon, SelectedImageKey = icon });
                g.Text = $"{name} ({g.Nodes.Count})";
                _tree.Nodes.Add(g);
                g.Expand();
            }
            Group("Spawns", "spawn", spec.Spawns, (o, i) => $"Spawn {i + 1}");
            Group("Areas", "area", spec.Areas, (o, i) => { var a = (Area)o; return (a.Name ?? $"Area {i + 1}") + (a.Music != null ? $"  ♪ {Path.GetFileNameWithoutExtension(a.Music)}" : ""); });
            Group("Warps", "warp", spec.Warps, (o, i) => { var w = (Warp)o; return (w.Name ?? $"Warp {i + 1}") + (w.To != null ? $"  → {w.To}" : ""); });
            Group("Event points", "event", spec.EventPoints.OrderBy(e => e.Event), (o, i) => ((EventPoint)o).Event.ToString());
        }
        _tree.EndUpdate();
        _syncing = false;
        SyncSelection();
        UpdateTitle();
    }

    TreeNode NodeOf(object o, TreeNodeCollection nodes)
    {
        foreach (TreeNode n in nodes)
        {
            if (n.Tag == o) return n;
            if (NodeOf(o, n.Nodes) is TreeNode c) return c;
        }
        return null;
    }

    void SyncSelection()
    {
        if (InvokeRequired) { BeginInvoke(SyncSelection); return; }
        _syncing = true;
        var o = _doc.Selected;
        _tree.SelectedNode = o == null ? null : NodeOf(o, _tree.Nodes);
        _props.SelectedObject = o;
        _snapshot = Snapshot();
        _syncing = false;
        _status.Text = o == null ? ToolHint() : $"{MapDoc.KindName(o)} picked. {ToolHint()}";
    }

    void ViewKey(KeyEventArgs k)
    {
        if (k.Control) return;  // (the menus' shortcuts)
        switch (k.KeyCode)
        {
            case Keys.Delete: _doc.DeleteSelected(); break;
            case Keys.Q: _doc.Tool = Tool.Select; break;
            case Keys.W: _doc.Tool = Tool.Move; break;
            case Keys.E: _doc.Tool = Tool.Rotate; break;
            case Keys.R when !k.Shift: _doc.Tool = Tool.Scale; break;
            case Keys.D1: _doc.Tool = Tool.Spawn; break;
            case Keys.D2: _doc.Tool = Tool.Area; break;
            case Keys.D3: _doc.Tool = Tool.Warp; break;
            case Keys.D4: _doc.Tool = Tool.Event; break;
            case Keys.Escape: _doc.Tool = Tool.Select; _doc.Selected = null; break;
            case Keys.G: _snap.Checked = !_snap.Checked; break;
            case Keys.OemOpenBrackets: _gridSize.SelectedIndex = Math.Max(0, _gridSize.SelectedIndex - 1); break;
            case Keys.OemCloseBrackets: _gridSize.SelectedIndex = Math.Min(_gridSize.Items.Count - 1, _gridSize.SelectedIndex + 1); break;
            case Keys.F when _doc.Selected is Placed p: _top.FrameOn(p); LookAt(p); break;
            case Keys.Home: FrameAll(); break;
        }
    }

    void LookAt(Placed p)
    {
        float size = p is Box b ? Math.Max(b.SizeX, Math.Max(b.SizeY, b.SizeZ)) : 50;
        _cam.Recentre(new Vector3(p.X, p.Y, p.Z), Math.Max(300, size * 2.2f));
    }

    void FrameAll()
    {
        var mid = (_view.Min + _view.Max) / 2;
        var ext = _view.Max - _view.Min;
        float r = Math.Max(ext.X, ext.Z) / 2;
        _cam.Look(new Vector3(mid.X, _view.Min.Y, mid.Z), r * 1.6f, 22, 0);
        _top.FrameAll();
    }

    // ---- building ----
    // the map mod: in the game's mods\maps (its folder named after the map, no spaces: the game's settings keep it) when
    // the game is known, else beside the map's file
    string ModFolder => new string((_doc.Spec?.Name ?? "Map").Where(char.IsLetterOrDigit).ToArray()) is { Length: > 0 } s ? s : "Map";
    string ModDir => File.Exists(_settings.GameExe ?? "")
        ? Path.Combine(Path.GetDirectoryName(_settings.GameExe), "mods", "maps", ModFolder)
        : Path.Combine(Path.GetDirectoryName(_doc.Path), "mod");

    void RunBusy(string what, Action work, Action done)
    {
        if (_building) return;
        _building = true;
        _progress.Visible = true;
        Cursor = Cursors.AppStarting;
        Log($"---- {what}");
        var sw = Stopwatch.StartNew();
        Task.Run(() =>
        {
            Exception err = null;
            try { work(); } catch (Exception e) { err = e; }
            BeginInvoke(() =>
            {
                _building = false;
                _progress.Visible = false;
                Cursor = Cursors.Default;
                if (err != null) { Log($"FAILED: {err.Message}\r\n{err.StackTrace}"); _status.Text = $"{what}: failed (see the messages below)"; return; }
                Log($"---- done in {sw.Elapsed.TotalSeconds:F0} s");
                _status.Text = $"{what}: done";
                done?.Invoke();
            });
        });
    }

    void Build(bool play)
    {
        if (_doc.Spec == null || _building) return;
        if (!Save()) return;
        if (!File.Exists(Path.Combine(_settings.CityDir ?? "", "GrCity1.dat")) && !AskCityDir()) return;
        if (play && !File.Exists(_settings.GameExe ?? "") && !AskGame()) return;
        string outDir = ModDir, specPath = _doc.Path, cityDir = _settings.CityDir;
        _status.Text = "Building: the collision, the minimaps and the music are made now...";
        RunBusy($"building {_doc.Spec.Name} into {outDir}", () =>
        {
            // (a fresh copy: the builder changes what it is given)
            if (Directory.Exists(outDir) && File.Exists(Path.Combine(outDir, "mod.json"))) Directory.Delete(outDir, true);
            Directory.CreateDirectory(outDir);
            var spec = MapSpec.Load(specPath);
            MapBuilder.Build(spec, cityDir, outDir, s => BeginInvoke(() => Log(s)));
            File.WriteAllText(Path.Combine(outDir, "mod.json"), JsonSerializer.Serialize(new { name = spec.Name, kind = "citytrial-map", version = 1 }, JsonOut));
        }, () => { if (play) Play(outDir); });
    }

    void Play(string modDir)
    {
        // (the map chosen in the game's settings, as its Game Settings screen's Stage row would)
        var dir = Path.GetDirectoryName(_settings.GameExe);
        var ini = Path.Combine(dir, "kirbyairride-settings.ini");
        var lines = File.Exists(ini) ? File.ReadAllLines(ini).Where(l => !l.TrimStart().StartsWith("city_map")).ToList() : new List<string>();
        lines.Add($"city_map = {Path.GetFileName(modDir)}");
        File.WriteAllLines(ini, lines);
        Process.Start(new ProcessStartInfo(_settings.GameExe) { UseShellExecute = false, WorkingDirectory = dir });
        Log($"the game started, Stage: {_doc.Spec.Name} (City Trial > Game Settings changes it)");
    }

    bool AskCityDir()
    {
        using var d = new FolderBrowserDialog { Description = "The game's files: the folder with GrCity1.dat, GrCity1Model.dat, IfAll1c.dat (an extracted disc)", UseDescriptionForTitle = true, SelectedPath = _settings.CityDir ?? "" };
        if (d.ShowDialog(this) != DialogResult.OK) return false;
        if (!File.Exists(Path.Combine(d.SelectedPath, "GrCity1.dat"))) { MessageBox.Show(this, "No GrCity1.dat in that folder.", Text); return false; }
        _settings.CityDir = d.SelectedPath; _settings.Save();
        return true;
    }

    bool AskGame()
    {
        using var d = new OpenFileDialog { Filter = "kirbyairride.exe|kirbyairride.exe|Programs|*.exe", Title = "The game" };
        if (d.ShowDialog(this) != DialogResult.OK) return false;
        _settings.GameExe = d.FileName; _settings.Save();
        return true;
    }

    void Log(string s) => _log.AppendText(s + "\r\n");
}

// what the editor remembers between runs (%APPDATA%\KarMapEditor\settings.json)
public class Settings
{
    public string CityDir { get; set; }
    public string GameExe { get; set; }
    public string LastMap { get; set; }
    public string SohArchive { get; set; }

    static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "KarMapEditor", "settings.json");

    public static Settings Load()
    {
        Settings s = null;
        try { if (File.Exists(FilePath)) s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)); } catch { }
        s ??= new Settings();
        if (s.CityDir == null)
        {
            // the source tree's extracted disc, when the editor runs from it
            for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "orig", "disc", "GrCity1.dat"))) { s.CityDir = Path.Combine(d.FullName, "orig", "disc"); break; }
        }
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }
}
