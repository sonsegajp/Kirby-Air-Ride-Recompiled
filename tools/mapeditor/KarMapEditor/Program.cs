// The City Trial map editor: a map's model in 3D with what is placed on it (spawns, areas, warps), edited and built
// into a map mod. (KarMapTool does the building; this is its window.)
using System.Globalization;

namespace KarMapEditor;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetHighDpiMode(HighDpiMode.SystemAware);
        Application.SetCompatibleTextRenderingDefault(false);
        Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, e) => MessageBox.Show(e.Exception.ToString(), "Map Editor: something went wrong", MessageBoxButtons.OK, MessageBoxIcon.Error);
        HSDRawViewer.Rendering.OpenTKResources.Init();
        Application.Run(new EditorForm(args.Length > 0 ? args[0] : null));
    }
}
