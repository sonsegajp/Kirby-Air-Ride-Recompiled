// The map's own settings as the Map tab shows them: grouped, described, with pickers for files and colours.
using System.ComponentModel;
using System.Drawing.Design;
using System.Windows.Forms.Design;

namespace KarMapEditor;

public class MapSettings
{
    readonly MapSpec _s;
    public MapSettings(MapSpec spec) { _s = spec; _s.Fog ??= new FogSettings(); }

    [Category("1 Map"), DisplayName("Name"), Description("The map's name, as the game's Stage setting shows it.")]
    public string Name { get => _s.Name; set => _s.Name = value; }

    [Category("2 Model"), DisplayName("Model"), Description("What the map is made of: .gltf .glb .obj .fbx .dae .smd. Its collision is made from it when the map is built."),
     Editor(typeof(ModelFileEditor), typeof(UITypeEditor))]
    public string Model { get => _s.Model; set => _s.Model = value; }

    [Category("2 Model"), DisplayName("Scale"), Description("Model units to the game's (the city is about 2600 across).")]
    public float Scale { get => _s.Scale; set => _s.Scale = value; }

    [Category("2 Model"), DisplayName("Z is up"), Description("The model's up axis is Z (many CAD and Source exports).")]
    public bool ZUp { get => _s.ZUp; set => _s.ZUp = value; }

    [Category("2 Model"), DisplayName("Centre it"), Description("Moved to the middle of the stage, where its sky is.")]
    public bool Center { get => _s.Center; set => _s.Center = value; }

    [Category("2 Model"), DisplayName("Collision (optional)"), Description("Ridden on instead of the model itself: a simpler model of its own. Empty: the model (automatic)."),
     Editor(typeof(ModelFileEditor), typeof(UITypeEditor))]
    public string Collision { get => _s.Collision; set => _s.Collision = string.IsNullOrWhiteSpace(value) ? null : value; }

    [Category("3 Sky"), DisplayName("Panorama"), Description("The sky all round: a picture of the whole horizon (its bottom edge the horizon)."),
     Editor(typeof(ImageFileEditor), typeof(UITypeEditor))]
    public string SkyPanorama { get => _s.SkyPanorama; set => _s.SkyPanorama = string.IsNullOrWhiteSpace(value) ? null : value; }

    [Category("3 Sky"), DisplayName("Overhead"), Description("The sky straight up."), Editor(typeof(ImageFileEditor), typeof(UITypeEditor))]
    public string SkyTop { get => _s.SkyTop; set => _s.SkyTop = string.IsNullOrWhiteSpace(value) ? null : value; }

    [Category("4 Fog"), DisplayName("Fog"), Description("Fog in the distance (as the city has). Off: none.")]
    public bool FogEnabled { get => _s.Fog.Enabled; set => _s.Fog.Enabled = value; }

    [Category("4 Fog"), DisplayName("Starts at"), Description("Where the fog begins (game units from the camera). Empty: the city's.")]
    public float? FogStart { get => _s.Fog.Start; set => _s.Fog.Start = value; }

    [Category("4 Fog"), DisplayName("Thickest at"), Description("Where the fog is complete. Empty: the city's.")]
    public float? FogEnd { get => _s.Fog.End; set => _s.Fog.End = value; }

    [Category("4 Fog"), DisplayName("Colour"), Description("The fog's colour. Empty: the city's.")]
    public Color FogColor
    {
        get => string.IsNullOrEmpty(_s.Fog.Color) ? Color.Empty : ColorTranslator.FromHtml(_s.Fog.Color);
        set => _s.Fog.Color = value.IsEmpty ? null : $"#{value.R:X2}{value.G:X2}{value.B:X2}";
    }

    [Category("5 Items"), DisplayName("Item spots"), Description("Boxes and power-ups: how many spots, scattered over the floor.")]
    public int ItemSpots { get => _s.ItemSpots; set => _s.ItemSpots = Math.Clamp(value, 10, 2000); }

    [Category("5 Items"), DisplayName("Random seed"), Description("Change it for another scattering of items, machines and event points.")]
    public int Seed { get => _s.Seed; set => _s.Seed = value; }
}

public class ModelFileEditor : FileNameEditor
{
    protected override void InitializeDialog(OpenFileDialog d)
    {
        base.InitializeDialog(d);
        d.Filter = "Models|*.gltf;*.glb;*.obj;*.fbx;*.dae;*.smd|All files|*.*";
    }
}

public class ImageFileEditor : FileNameEditor
{
    protected override void InitializeDialog(OpenFileDialog d)
    {
        base.InitializeDialog(d);
        d.Filter = "Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*";
    }
}
