// The viewer's camera, turning about a point of the map (the map's middle, a picked object, a double-clicked spot)
// rather than about the world's origin.
using HSDRawViewer.Rendering;
using OpenTK.Mathematics;

namespace KarMapEditor;

public class MapCamera : Camera
{
    Vector3 _pivot;
    public Vector3 Pivot { get => _pivot; set { _pivot = value; Translation = Translation; } }

    protected override void UpdateModelViewMatrix()
    {
        modelViewMatrix = Matrix4.CreateTranslation(-_pivot) * Matrix4.CreateScale(Scale) * rotationMatrix * translationMatrix;
    }

    public void Look(Vector3 pivot, float distance, float pitchDegrees, float yawDegrees)
    {
        _pivot = pivot;
        RotationXDegrees = pitchDegrees;
        RotationYDegrees = yawDegrees;
        Translation = new Vector3(0, 0, -distance);
    }

    // a new point to turn about, the view where it is (as seen now, from as far)
    public void Recentre(Vector3 pivot, float? distance = null)
    {
        _pivot = pivot;
        Translation = new Vector3(0, 0, -(distance ?? Math.Abs(Translation.Z)));
    }
}
