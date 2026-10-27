// The map being edited, shared by the views: its spec (map.json), its model, what is picked, the tool in hand, the grid,
// and undo (the spec as it was, kept as JSON before each change).
using System.Text.Json;
using System.Text.Json.Serialization;
using IONET.Core;
using NVec3 = System.Numerics.Vector3;

namespace KarMapEditor;

public enum Tool { Select, Move, Rotate, Scale, Spawn, Area, Warp, Event }

public class MapDoc
{
    public MapSpec Spec { get; private set; }
    public string Path { get; set; }
    public IOScene Scene { get; set; }
    public bool Modified { get; set; }

    object _selected;
    public object Selected
    {
        get => _selected;
        set { if (_selected == value) return; _selected = value; SelectionChanged?.Invoke(); }
    }
    Tool _tool = Tool.Select;
    public Tool Tool { get => _tool; set { _tool = value; ToolChanged?.Invoke(); } }
    public EventKind PlaceEvent { get; set; } = EventKind.DynaBlade;
    public float Grid { get; set; } = 32;
    public bool Snap { get; set; } = true;

    public event Action Changed;           // something placed moved or changed
    public event Action StructureChanged;  // something added or removed, or another map
    public event Action SelectionChanged;
    public event Action ToolChanged;

    // the ground under a point (the 3D view's model): set by the view
    public Func<NVec3, float, float> GroundBelow { get; set; } = (p, f) => f;

    public void Load(MapSpec spec, string path, IOScene scene)
    {
        Spec = spec; Path = path; Scene = scene;
        _undo.Clear(); _redo.Clear();
        Modified = false;
        _selected = null;
        StructureChanged?.Invoke();
        SelectionChanged?.Invoke();
    }

    public void Touch() { Modified = true; Changed?.Invoke(); }
    public void TouchStructure() { Modified = true; StructureChanged?.Invoke(); Changed?.Invoke(); }

    // ---- undo ----
    static readonly JsonSerializerOptions Json = new() { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };
    readonly List<string> _undo = new(), _redo = new();
    public bool CanUndo => _undo.Count > 0;
    public bool CanRedo => _redo.Count > 0;
    public void Checkpoint()
    {
        if (Spec == null) return;
        _undo.Add(JsonSerializer.Serialize(Spec, Json));
        if (_undo.Count > 200) _undo.RemoveAt(0);
        _redo.Clear();
    }
    // (the spec as it was before a change made elsewhere - a property edited - kept for undo)
    public void CheckpointFrom(string json)
    {
        if (json == null) { Checkpoint(); return; }
        _undo.Add(json);
        if (_undo.Count > 200) _undo.RemoveAt(0);
        _redo.Clear();
    }
    public void Undo() => Swap(_undo, _redo);
    public void Redo() => Swap(_redo, _undo);
    void Swap(List<string> from, List<string> to)
    {
        if (from.Count == 0 || Spec == null) return;
        to.Add(JsonSerializer.Serialize(Spec, Json));
        var json = from[^1];
        from.RemoveAt(from.Count - 1);
        int sel = IndexOf(_selected, out var list);
        Spec = JsonSerializer.Deserialize<MapSpec>(json, Json);
        _selected = sel >= 0 ? ListOf(list)?.ElementAtOrDefault(sel) : null;
        Modified = true;
        StructureChanged?.Invoke();
        SelectionChanged?.Invoke();
        Changed?.Invoke();
    }
    int IndexOf(object o, out int list)
    {
        list = o switch { Area => 1, Warp => 2, EventPoint => 3, Placed => 0, _ => -1 };
        return list < 0 ? -1 : ListOf(list).IndexOf(o);
    }
    List<object> ListOf(int list) => list switch
    {
        0 => Spec.Spawns.Cast<object>().ToList(),
        1 => Spec.Areas.Cast<object>().ToList(),
        2 => Spec.Warps.Cast<object>().ToList(),
        3 => Spec.EventPoints.Cast<object>().ToList(),
        _ => null,
    };

    // ---- what is placed ----
    public IEnumerable<Placed> All()
    {
        if (Spec == null) yield break;
        foreach (var a in Spec.Areas) yield return a;
        foreach (var w in Spec.Warps) yield return w;
        foreach (var s in Spec.Spawns) yield return s;
        foreach (var e in Spec.EventPoints) yield return e;
    }

    public float SnapTo(float v) => Snap && Grid > 0 ? MathF.Round(v / Grid) * Grid : v;

    // a new object of the tool's kind at a point on the ground
    public Placed Place(Tool tool, NVec3 ground)
    {
        if (Spec == null) return null;
        Checkpoint();
        Placed p = tool switch
        {
            Tool.Area => new Area { SizeX = 1500, SizeY = 800, SizeZ = 1500, Minimap = true, Name = Unique("area") },
            Tool.Warp => new Warp { SizeX = 150, SizeY = 80, SizeZ = 60, Visible = false, Name = Unique("warp") },
            Tool.Event => new EventPoint { Event = PlaceEvent },
            _ => new Placed(),
        };
        p.X = SnapTo(ground.X); p.Z = SnapTo(ground.Z);
        float g = GroundBelow(new NVec3(p.X, ground.Y + 50, p.Z), ground.Y);
        p.Y = g + (p is Box b ? b.SizeY / 2 : 10);
        switch (p)
        {
            case Area a: Spec.Areas.Add(a); break;
            case Warp w: w.ToX = p.X; w.ToY = p.Y; w.ToZ = p.Z + 300; Spec.Warps.Add(w); break;
            case EventPoint e: Spec.EventPoints.Add(e); break;
            default: Spec.Spawns.Add(p); break;
        }
        TouchStructure();
        Selected = p;
        return p;
    }

    public string Unique(string stem)
    {
        var names = Spec.Areas.Select(a => a.Name).Concat(Spec.Warps.Select(w => w.Name)).ToHashSet();
        for (int i = 1; ; i++) if (!names.Contains($"{stem}{i}")) return $"{stem}{i}";
    }

    public void DeleteSelected()
    {
        if (_selected is not Placed p) return;
        Checkpoint();
        switch (p)
        {
            case Area a: Spec.Areas.Remove(a); break;
            case Warp w: Spec.Warps.Remove(w); break;
            case EventPoint e: Spec.EventPoints.Remove(e); break;
            default: Spec.Spawns.Remove(p); break;
        }
        Selected = null;
        TouchStructure();
    }

    public void DuplicateSelected()
    {
        if (_selected is not Placed p) return;
        Checkpoint();
        var copy = (Placed)JsonSerializer.Deserialize(JsonSerializer.Serialize(p, p.GetType(), Json), p.GetType(), Json);
        copy.X += Math.Max(Grid, 40); copy.Z += Math.Max(Grid, 40);
        if (copy is Box b && b.Name != null) b.Name = Unique(new string(b.Name.TrimEnd("0123456789".ToCharArray())));
        switch (copy)
        {
            case Area a: Spec.Areas.Add(a); break;
            case Warp w: Spec.Warps.Add(w); break;
            case EventPoint e: Spec.EventPoints.Add(e); break;
            default: Spec.Spawns.Add(copy); break;
        }
        TouchStructure();
        Selected = copy;
    }

    // a warp's destination: its point, or in front of the box it names
    public (NVec3, float) Destination(Warp w)
    {
        if (!string.IsNullOrEmpty(w.To))
        {
            Box b = (Box)Spec.Warps.FirstOrDefault(x => x.Name == w.To) ?? Spec.Areas.FirstOrDefault(x => x.Name == w.To);
            if (b != null)
            {
                float r = b.Yaw * MathF.PI / 180f;
                return (b.Position + new NVec3(-MathF.Sin(r), 0, -MathF.Cos(r)) * (Math.Max(b.SizeX, b.SizeZ) / 2 + 60), b.Yaw);
            }
        }
        return (new NVec3(w.ToX, w.ToY, w.ToZ), w.ToYaw);
    }

    public static string KindName(object o) => o switch
    {
        Area => "Area",
        Warp => "Warp",
        EventPoint e => $"Event: {e.Event}",
        Placed => "Spawn",
        MapSpec => "Map",
        _ => "",
    };
}
