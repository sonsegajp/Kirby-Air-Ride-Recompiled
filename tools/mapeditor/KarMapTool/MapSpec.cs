// A City Trial map as the editor saves it (map.json): the model it is made of and what is placed on it. The builder
// (MapBuilder.cs) turns it into the game's stage files.
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Serialization;

public enum PadKind { Boost, Jump, Ability }

public class Placed
{
    public float X { get; set; }
    public float Y { get; set; }
    public float Z { get; set; }
    public float Yaw { get; set; }  // degrees, about the up axis (0: facing -Z, as the city's start positions)
    [JsonIgnore] public Vector3 Position => new(X, Y, Z);
}

public class Pad : Placed
{
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public PadKind Kind { get; set; }
}

// A box on the map (centre, size, turn about the up axis): where an area or a warp is.
public class Box : Placed
{
    public float SizeX { get; set; } = 100;
    public float SizeY { get; set; } = 100;
    public float SizeZ { get; set; } = 100;
    public string Name { get; set; }
}

// Part of the map with music of its own (a sub-area): its music plays while you are in it, the map's again when you leave.
public class Area : Box
{
    public string Music { get; set; }        // a sound file (.wav .ogg .mp3), or a game's .hps
    public float LoopStart { get; set; }     // seconds: where the music starts again when it ends
    public bool Minimap { get; set; }        // a minimap of its own (shown while the first rider is in it)
}

// City Trial's events that happen at points of the map (GrCity1Event.dat: each event's points, a range of the stage's
// event list): their numbers are the game's. Events not placed get points of their own on the floor.
public enum EventKind
{
    DynaBlade = 0,          // where Dyna Blade swoops down
    Tac = 1,                // where Tac appears
    Meteor = 2,             // where the meteors land
    Pillar = 3,             // where the pillars rise
    RestorationArea = 5,    // the healing rings
    MachineFormation = 11,  // where the machines line up
    Ufo = 12,               // where the UFO shows up
}

// A point where an event happens.
public class EventPoint : Placed
{
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public EventKind Event { get; set; }
}

// The stage's fog (GrCity1.dat's fog node): off, or its start and end distance and colour (null: the city's).
public class FogSettings
{
    public bool Enabled { get; set; } = true;
    public float? Start { get; set; }
    public float? End { get; set; }
    public string Color { get; set; }  // "#RRGGBB"
}

// How the minimap is drawn (Minimap.cs).
public class MinimapStyle
{
    public bool Textured { get; set; } = true;      // the map's own textures, from above; else one colour, shaded
    public string LandColor { get; set; } = "#7FB070";  // (untextured)
    public bool Outline { get; set; } = true;       // a line round the land
    public string OutlineColor { get; set; } = "#FFFFFF";
    public float Brightness { get; set; } = 1.0f;
    public float Contrast { get; set; } = 1.15f;
    public float Opacity { get; set; } = 0.9f;
    public float Margin { get; set; } = 1.04f;      // room round the map (1: none)
}

// A warp: ride into its box and you are put at its destination (a point, or another warp's name), facing its way.
public class Warp : Box
{
    public string To { get; set; }           // another warp's (or any box's) name: its destination is in front of it
    public float ToX { get; set; }
    public float ToY { get; set; }
    public float ToZ { get; set; }
    public float ToYaw { get; set; }
    public bool Visible { get; set; } = true;  // a ring drawn at it
}

public class MapSpec
{
    public string Name { get; set; } = "Custom City";
    public string Model { get; set; }        // a model file (.obj .fbx .dae .gltf .glb .smd), or a Valve .bsp (later)
    public string Collision { get; set; }    // what is ridden on, if not the model itself: a model file of its own
    public float Scale { get; set; } = 1f;   // model units -> stage units (the city is about 2600 across)
    public bool ZUp { get; set; }            // the model's up axis is Z (Valve maps, many CAD exports)
    public bool Center { get; set; } = true; // moved to the middle of the stage (where its sky is)
    public string SkyPanorama { get; set; }  // the sky all round: a picture of the whole horizon (its bottom the horizon)
    public string SkyTop { get; set; }       // the sky overhead
    public List<Placed> Spawns { get; set; } = new();
    public List<Pad> Pads { get; set; } = new();
    public string Music { get; set; }        // the map's music (in City Trial's place): a sound file, or a game's .hps
    public float MusicLoopStart { get; set; }  // seconds
    public List<Area> Areas { get; set; } = new();
    public List<Warp> Warps { get; set; } = new();
    public List<EventPoint> EventPoints { get; set; } = new();
    public FogSettings Fog { get; set; } = new();
    public MinimapStyle MinimapStyle { get; set; } = new();
    public int ItemSpots { get; set; } = 300;  // boxes and power-ups: spots scattered over the floor
    public int Seed { get; set; } = 1;
    // (debugging the builder: keep the city's own part instead of making it from the model)
    public bool KeepCityCollision { get; set; }
    public bool KeepCityPositions { get; set; }
    public bool KeepCityModel { get; set; }
    public bool KeepCityZones { get; set; }
    public bool RegenCityTree { get; set; }
    public bool KeepCityMeshes { get; set; }
    public bool OnCityRoot { get; set; } = true;  // (the map's meshes on the city's root joint: the stage draws those)
    public bool ModelOnly { get; set; }

    public static MapSpec Load(string path)
    {
        var spec = JsonSerializer.Deserialize<MapSpec>(File.ReadAllText(path), new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        var dir = Path.GetDirectoryName(Path.GetFullPath(path));
        if (spec.Model != null && !Path.IsPathRooted(spec.Model)) spec.Model = Path.Combine(dir, spec.Model);
        if (spec.Collision != null && !Path.IsPathRooted(spec.Collision)) spec.Collision = Path.Combine(dir, spec.Collision);
        if (spec.SkyPanorama != null && !Path.IsPathRooted(spec.SkyPanorama)) spec.SkyPanorama = Path.Combine(dir, spec.SkyPanorama);
        if (spec.SkyTop != null && !Path.IsPathRooted(spec.SkyTop)) spec.SkyTop = Path.Combine(dir, spec.SkyTop);
        string Rooted(string p) => p == null || Path.IsPathRooted(p) ? p : Path.Combine(dir, p);
        spec.Music = Rooted(spec.Music);
        foreach (var a in spec.Areas) a.Music = Rooted(a.Music);
        return spec;
    }
}
