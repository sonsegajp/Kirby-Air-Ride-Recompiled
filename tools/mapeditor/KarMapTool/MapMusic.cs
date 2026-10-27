// A map's music as the game's: any sound file (.wav .ogg .mp3, or a game's .hps as it is) made into a looping .hps
// stream (GameCube DSP-ADPCM, 32 kHz stereo, as City Trial's own city.hps) with VGAudio (MIT). The game then plays it
// as its own music: its volume settings, its fades, its loop.
using NLayer;
using NVorbis;
using VGAudio.Containers.Hps;
using VGAudio.Containers.Wave;
using VGAudio.Formats;
using VGAudio.Formats.Pcm16;

public static class MapMusic
{
    const int Rate = 32000;

    public static byte[] ToHps(string path, float loopStartSeconds, Action<string> log)
    {
        if (path.EndsWith(".hps", StringComparison.OrdinalIgnoreCase)) return File.ReadAllBytes(path);
        var (left, right, rate) = Decode(path);
        var l = Resample(left, rate); var r = Resample(right, rate);
        int loop = Math.Clamp((int)(loopStartSeconds * Rate), 0, Math.Max(0, l.Length - 1));
        var pcm = new Pcm16Format(new[] { l, r }, Rate).WithLoop(true, loop, l.Length);
        using var ms = new MemoryStream();
        new HpsWriter().WriteToStream(new AudioData(pcm), ms, new HpsConfiguration());
        log($"music: {Path.GetFileName(path)}, {l.Length / (float)Rate:F1} s, looping from {loop / (float)Rate:F1} s");
        return Reblock(ms.ToArray(), l.Length, loop);
    }

    static uint Be32(byte[] d, int o) => (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    static void Put32(byte[] d, int o, uint v) { d[o] = (byte)(v >> 24); d[o + 1] = (byte)(v >> 16); d[o + 2] = (byte)(v >> 8); d[o + 3] = (byte)v; }
    static void Put16(byte[] d, int o, int v) { d[o] = (byte)(v >> 8); d[o + 1] = (byte)v; }

    // The stream laid out as the game's own (city.hps): blocks of 0x10000 bytes of sound (0x8000 a channel, left then
    // right) after a 0x20-byte head - its size, its last nibble, the next block's place, and each channel's decoder
    // state where it starts (predictor/scale, two history samples). VGAudio's blocks are a little smaller, and the game's
    // streamer takes them 0x10000 at a time: its own size, or the music stops and the game waits on it.
    static byte[] Reblock(byte[] vg, int samples, int loopSample)
    {
        const int Block = 0x10000, Half = Block / 2, Head = 0x20;
        int channels = (int)Be32(vg, 0x0C);
        // each channel's whole ADPCM stream, from VGAudio's blocks
        var streams = Enumerable.Range(0, channels).Select(_ => new List<byte>()).ToArray();
        for (int off = 0x80, n = 0; n < 100000; n++)
        {
            int size = (int)Be32(vg, off), next = (int)Be32(vg, off + 8);
            int per = size / channels;
            for (int c = 0; c < channels; c++) streams[c].AddRange(vg.AsSpan(off + Head + c * per, per).ToArray());
            if (next <= off) break;  // (the last: it leads back to the loop)
            off = next;
        }
        // the decoder state at each byte of the stream we cut at (each frame: 8 bytes, 14 samples)
        short[][] coefs = Enumerable.Range(0, channels).Select(c => Enumerable.Range(0, 16).Select(i => (short)(vg[0x10 + c * 0x38 + 0x10 + i * 2] << 8 | vg[0x10 + c * 0x38 + 0x11 + i * 2])).ToArray()).ToArray();
        // (one pass a channel: its state at the start of each block of Half bytes)
        var at = new Dictionary<(int, int), (int, int)>();
        for (int ch = 0; ch < channels; ch++)
        {
            int h1 = 0, h2 = 0;
            var s = streams[ch];
            for (int f = 0; f <= s.Count; f += 8)
            {
                if (f % Half == 0) at[(ch, f)] = (h1, h2);
                if (f + 8 > s.Count) break;
                int ps = s[f], pred = ps >> 4 & 7, scale = 1 << (ps & 15);
                int c1 = coefs[ch][pred * 2], c2 = coefs[ch][pred * 2 + 1];
                for (int k = 0; k < 14; k++)
                {
                    int bb = s[f + 1 + k / 2], nib = k % 2 == 0 ? bb >> 4 : bb & 15;
                    if (nib >= 8) nib -= 16;
                    int v = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2 >> 11;
                    v = Math.Clamp(v, -32768, 32767);
                    h2 = h1; h1 = v;
                }
            }
        }
        (int h1, int h2) StateAt(int c, int byteOff) => at.TryGetValue((c, byteOff), out var st) ? st : (0, 0);
        (int h1, int h2) StateAtSlow(int c, int byteOff)
        {
            int h1 = 0, h2 = 0;
            var s = streams[c];
            for (int f = 0; f < byteOff; f += 8)
            {
                int ps = s[f], pred = ps >> 4 & 7, scale = 1 << (ps & 15);
                int c1 = coefs[c][pred * 2], c2 = coefs[c][pred * 2 + 1];
                for (int k = 0; k < 14; k++)
                {
                    int b = s[f + 1 + k / 2], nib = k % 2 == 0 ? b >> 4 : b & 15;
                    if (nib >= 8) nib -= 16;
                    int v = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2 >> 11;
                    v = Math.Clamp(v, -32768, 32767);
                    h2 = h1; h1 = v;
                }
            }
            return (h1, h2);
        }
        int perChannel = streams[0].Count;
        int blocks = (perChannel + Half - 1) / Half;
        var outp = new byte[0x80 + blocks * (Head + Block)];
        Array.Copy(vg, outp, 0x80);  // (the head: rate, channels, each channel's coefficients, as VGAudio wrote them)
        for (int c = 0; c < channels; c++) Put32(outp, 0x10 + c * 0x38, Block);  // (the largest block)
        int loopByte = loopSample / 14 * 8;
        int loopBlock = loopByte / Half;
        int pos = 0x80;
        int lastNibble = (samples - 1) / 14 * 16 + 2 + (samples - 1) % 14;  // (the last sample's nibble, in a channel's stream)
        var states = new (int ps, int h1, int h2)[channels];
        for (int b = 0; b < blocks; b++)
        {
            int start = b * Half, len = Math.Min(Half, perChannel - start);
            int size = len * channels;
            bool last = b == blocks - 1;
            Put32(outp, pos, (uint)size);
            Put32(outp, pos + 4, (uint)(last ? Math.Min(len * 2 - 1, lastNibble - start * 2) : len * 2 - 1));
            Put32(outp, pos + 8, (uint)(last ? 0x80 + loopBlock * (Head + Block) : pos + Head + size));
            for (int c = 0; c < channels; c++)
            {
                var (h1, h2) = StateAt(c, start);
                Put16(outp, pos + 0xC + c * 8, streams[c][start]);
                Put16(outp, pos + 0xC + c * 8 + 2, h1);
                Put16(outp, pos + 0xC + c * 8 + 4, h2);
                streams[c].CopyTo(start, outp, pos + Head + c * len, len);
            }
            pos += Head + size;
        }
        return outp[..pos];
    }

    // The file's samples, left and right (mono: both the same), and its rate.
    public static (float[] l, float[] r, int rate) Decode(string path)
    {
        var ext = Path.GetExtension(path).ToLowerInvariant();
        if (ext == ".ogg")
        {
            using var v = new VorbisReader(path);
            var all = new List<float>();
            var buf = new float[v.Channels * 4096];
            int n;
            while ((n = v.ReadSamples(buf, 0, buf.Length)) > 0) all.AddRange(buf.Take(n));
            return Split(all, v.Channels, v.SampleRate);
        }
        if (ext == ".mp3")
        {
            using var m = new MpegFile(path);
            var all = new List<float>();
            var buf = new float[m.Channels * 4096];
            int n;
            while ((n = m.ReadSamples(buf, 0, buf.Length)) > 0) all.AddRange(buf.Take(n));
            return Split(all, m.Channels, m.SampleRate);
        }
        if (ext == ".wav")
        {
            using var fs = File.OpenRead(path);
            var audio = new WaveReader().Read(fs);
            var pcm = audio.GetFormat<Pcm16Format>();
            var ch = pcm.Channels;
            float[] F(short[] s) => s.Select(x => x / 32768f).ToArray();
            return (F(ch[0]), F(ch.Length > 1 ? ch[1] : ch[0]), pcm.SampleRate);
        }
        throw new Exception($"{Path.GetFileName(path)}: music must be .wav, .ogg, .mp3 or .hps");
    }

    static (float[], float[], int) Split(List<float> inter, int channels, int rate)
    {
        int frames = inter.Count / channels;
        var l = new float[frames]; var r = new float[frames];
        for (int i = 0; i < frames; i++) { l[i] = inter[i * channels]; r[i] = inter[i * channels + (channels > 1 ? 1 : 0)]; }
        return (l, r, rate);
    }

    // To 32 kHz (cubic), as 16-bit.
    static short[] Resample(float[] s, int rate)
    {
        int n = (int)((long)s.Length * Rate / rate);
        var o = new short[n];
        double step = (double)rate / Rate;
        float At(int i) => s[Math.Clamp(i, 0, s.Length - 1)];
        for (int i = 0; i < n; i++)
        {
            double p = i * step;
            int k = (int)p; float t = (float)(p - k);
            float y0 = At(k - 1), y1 = At(k), y2 = At(k + 1), y3 = At(k + 2);
            float v = y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
            o[i] = (short)Math.Clamp((int)MathF.Round(v * 32767), -32768, 32767);
        }
        return o;
    }
}
