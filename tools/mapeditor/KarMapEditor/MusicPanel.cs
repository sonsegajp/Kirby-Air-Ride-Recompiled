// The map's music: the track played over the whole map (in City Trial's place), and each area's own (played while the
// first rider is in it). Any .wav, .ogg or .mp3: the builder turns them into the game's streams and packs them in the
// mod. Each with where it starts again when it ends, and a preview.
using System.Media;

namespace KarMapEditor;

public class MusicPanel : UserControl
{
    readonly MapDoc _doc;
    readonly TableLayoutPanel _table = new() { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 5, Padding = new Padding(6) };
    readonly Label _note = new() { Dock = DockStyle.Top, AutoSize = false, Height = 54, Padding = new Padding(8, 6, 8, 0) };
    SoundPlayer _player;
    string _playing;

    public MusicPanel(MapDoc doc)
    {
        _doc = doc;
        AutoScroll = true;
        _note.Text = "Music in .wav, .ogg or .mp3 - packed into the map when it is built. Loop: where the music starts again (seconds). An area's music plays while you are in it.";
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 34));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 34));
        Controls.Add(_table);
        Controls.Add(_note);
        doc.StructureChanged += Rebuild;
        Disposed += (_, _) => StopPreview();
    }

    void Rebuild()
    {
        if (InvokeRequired) { BeginInvoke(Rebuild); return; }
        _table.SuspendLayout();
        _table.Controls.Clear();
        _table.RowStyles.Clear();
        _table.RowCount = 0;
        if (_doc.Spec != null)
        {
            var spec = _doc.Spec;
            Row("Whole map", () => spec.Music, v => spec.Music = v, () => spec.MusicLoopStart, v => spec.MusicLoopStart = v, true);
            foreach (var a in spec.Areas)
            {
                var area = a;
                Row(area.Name ?? "area", () => area.Music, v => area.Music = v, () => area.LoopStart, v => area.LoopStart = v, false);
            }
        }
        _table.ResumeLayout();
        Theme.Apply(this);
    }

    void Row(string name, Func<string> get, Action<string> set, Func<float> getLoop, Action<float> setLoop, bool bold)
    {
        int r = _table.RowCount++;
        _table.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        var label = new Label { Text = name, AutoEllipsis = true, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
        if (bold) label.Font = new Font(Font, FontStyle.Bold);
        var path = new TextBox { Text = get() == null ? "" : Path.GetFileName(get()), ReadOnly = true, Dock = DockStyle.Fill, PlaceholderText = "(none)" };
        var tip = new ToolTip();
        tip.SetToolTip(path, get() ?? "no music: the map's own plays");
        var browse = new Button { Image = Theme.Icon(Theme.GOpen, 16), Dock = DockStyle.Fill };
        tip.SetToolTip(browse, "Choose a sound file (.wav .ogg .mp3)");
        var loop = new NumericUpDown { DecimalPlaces = 1, Maximum = 3600, Increment = 0.5m, Value = (decimal)Math.Clamp(getLoop(), 0, 3600), Dock = DockStyle.Fill };
        tip.SetToolTip(loop, "Loop start (seconds)");
        var play = new Button { Image = Theme.Icon(_playing == get() && get() != null ? Theme.GStop : Theme.GPlay, 16), Dock = DockStyle.Fill, Enabled = get() != null };
        tip.SetToolTip(play, "Preview (30 seconds)");
        browse.Click += (_, _) =>
        {
            using var d = new OpenFileDialog { Filter = "Music|*.wav;*.ogg;*.mp3|All files|*.*", Title = $"Music: {name}" };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            _doc.Checkpoint();
            set(d.FileName);
            _doc.Touch();
            Rebuild();
        };
        path.DoubleClick += (_, _) =>
        {
            if (get() == null) return;
            _doc.Checkpoint();
            set(null);
            _doc.Touch();
            Rebuild();
        };
        loop.ValueChanged += (_, _) => { _doc.Checkpoint(); setLoop((float)loop.Value); _doc.Touch(); };
        play.Click += async (_, _) =>
        {
            var file = get();
            if (file == null) return;
            if (_playing == file) { StopPreview(); Rebuild(); return; }
            StopPreview();
            play.Enabled = false;
            try
            {
                var wav = await Task.Run(() => PreviewWav(file));
                _player = new SoundPlayer(wav);
                _player.Play();
                _playing = file;
            }
            catch (Exception e) { MessageBox.Show(this, e.Message, "Could not play it"); }
            Rebuild();
        };
        _table.Controls.Add(label, 0, r);
        _table.Controls.Add(path, 1, r);
        _table.Controls.Add(browse, 2, r);
        _table.Controls.Add(loop, 3, r);
        _table.Controls.Add(play, 4, r);
    }

    void StopPreview()
    {
        _player?.Stop();
        _player?.Dispose();
        _player = null;
        _playing = null;
    }

    // the first 30 seconds as a 16-bit stereo WAV (decoded as the builder does)
    static string PreviewWav(string file)
    {
        var (l, r, rate) = MapMusic.Decode(file);
        int n = Math.Min(l.Length, rate * 30);
        var path = Path.Combine(Path.GetTempPath(), "karmap_preview.wav");
        using var w = new BinaryWriter(File.Create(path));
        w.Write("RIFF"u8.ToArray()); w.Write(36 + n * 4); w.Write("WAVEfmt "u8.ToArray());
        w.Write(16); w.Write((short)1); w.Write((short)2); w.Write(rate); w.Write(rate * 4); w.Write((short)4); w.Write((short)16);
        w.Write("data"u8.ToArray()); w.Write(n * 4);
        for (int i = 0; i < n; i++)
        {
            w.Write((short)Math.Clamp(l[i] * 32767, -32768, 32767));
            w.Write((short)Math.Clamp((r ?? l)[i] * 32767, -32768, 32767));
        }
        return path;
    }
}
