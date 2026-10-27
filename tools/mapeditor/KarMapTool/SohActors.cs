// The scenery of OoT's scenes that the game draws as actors (its rooms' actor lists, and the doors of its scene's list):
// trees, bushes, grass, rocks and boulders, signs, fences, gravestones, crates, pots, torches, gates, bridges, statues,
// the windmill's sails, the lake's water and the like - each drawn as its actor's draw code draws it (its display lists,
// its scale, its turn, the colours it sets), with the collision the actor gives the game (its own, a DynaPoly one) or,
// for what the game only bumps Link off (its cylinder: trees, boulders, signs, stones), a solid as wide and tall as that
// cylinder. Doors are drawn but not solid: the map's exits are its warps. Left out: enemies, people and animals, Link,
// items and what holds them (chests, pots of rupees are kept as pots), effects, and the actors nobody sees (switches,
// spawners, sounds, paths, cutscenes, message spots).
//
// Where an actor looks different by Link's age or the time of day, it is drawn as by day and as adult Link sees it (the
// mended bridge of Gerudo Valley, the open gate to Death Mountain), except what only the frozen adult Zora's Domain has.
using System.Numerics;

public static partial class SohScene
{
    class Scenery
    {
        readonly O2rArchive A;
        readonly Rsp R;
        readonly List<(string path, Matrix4x4 xf)> Coll;
        readonly Action<string> Log;
        public readonly List<(Vector3, Vector3, Vector3)> Solids = new();  // (in OoT units, placed in the map)
        public int Triangles;                                               // the model's triangles the actors drew
        readonly SortedDictionary<string, (int n, int tris, int coll)> kinds = new();
        readonly SortedDictionary<string, int> skipped = new();

        // the place being done: its name, where it is set, its floor (the scene's collision, in its own units)
        string part;
        Matrix4x4 partXf;
        List<(Vector3 a, Vector3 b, Vector3 c)> floor = new();

        public Scenery(O2rArchive a, Rsp rsp, List<(string, Matrix4x4)> coll, Action<string> log) { A = a; R = rsp; Coll = coll; Log = log; }

        // ---- the actors' names, for what is logged (include/tables/actor_table.h) ----
        static readonly Dictionary<int, string> Kept = new()
        {
            [0x009] = "En_Door (doors)", [0x026] = "En_Hata (flagpoles)", [0x039] = "En_A_Obj (signposts)", [0x03E] = "Bg_Treemouth (the Deku Tree's mouth)",
            [0x04A] = "Bg_Spot00_Hanebasi (the drawbridge)", [0x05E] = "Obj_Syokudai (torches)", [0x06E] = "Bg_Mjin (warp pads)",
            [0x077] = "En_Wood02 (trees, bushes)", [0x09C] = "Bg_Spot02_Objects (the graveyard's tombs)", [0x09D] = "Bg_Haka (gravestones)",
            [0x0B8] = "Bg_Spot09_Obj (Gerudo Valley's bridge, tent)", [0x0B9] = "Bg_Spot18_Obj (Goron City's statue)",
            [0x0CD] = "Bg_Spot16_Bombstone (Dodongo's Cavern's boulder)", [0x0D1] = "Bg_Spot08_Iceblock (Zora's Fountain's ice)",
            [0x0D5] = "Bg_Spot06_Objects (Lake Hylia's water, gate)", [0x0E5] = "Bg_Spot16_Doughnut (Death Mountain's cloud ring)",
            [0x0F8] = "Bg_Spot15_Saku (the castle's fence)", [0x100] = "Bg_Gate_Shutter (Kakariko's gate)", [0x102] = "Bg_Spot01_Fusya (the windmill's sails)",
            [0x103] = "Bg_Spot01_Idohashira (the well's beam)", [0x104] = "Bg_Spot01_Idomizu (the well's water)", [0x107] = "Bg_Spot15_Rrbox (milk crates)",
            [0x108] = "Bg_Umajump (horse fences)", [0x111] = "Obj_Tsubo (pots)", [0x11F] = "Bg_Spot03_Taki (Zora's River's waterfall)",
            [0x120] = "Bg_Spot07_Taki (Zora's Domain's waterfalls)", [0x125] = "En_Kusa (bushes, grass)", [0x127] = "Obj_Bombiwa (boulders)",
            [0x12D] = "Obj_Hsblock (hookshot posts)", [0x141] = "En_Kanban (signs)", [0x14E] = "En_Ishi (rocks)", [0x14F] = "Obj_Hana (flowers)",
            [0x151] = "Obj_Mure2 (rings of bushes, rocks)", [0x15C] = "Bg_Spot18_Basket (Goron City's urn)", [0x172] = "Door_Gerudo (cell doors)",
            [0x18D] = "Bg_Spot05_Soko (the meadow's walls)", [0x190] = "Bg_Spot12_Gate (the fortress's gate)", [0x191] = "Bg_Spot12_Saku (the fortress's shutter)",
            [0x19D] = "Bg_Spot01_Objects2 (Kakariko's building site, signs)", [0x19E] = "Obj_Comb (beehives)", [0x1A0] = "Obj_Kibako2 (crates)",
            [0x1B9] = "En_Gs (gossip stones)", [0x1C4] = "Bg_Spot18_Shutter (Goron City's doors)", [0x1D2] = "Obj_Hamishi (bronze boulders)",
        };

        // what is left out, and why (each logged by its name)
        static readonly Dictionary<int, string> Left = new()
        {
            [0x008] = "En_Light (a fire)", [0x00A] = "En_Box (a chest)", [0x00E] = "En_Okuta (an enemy)", [0x015] = "En_Item00 (an item)",
            [0x019] = "En_Niw (a cucco)", [0x01B] = "En_Tite (an enemy)", [0x01D] = "En_Peehat (an enemy)", [0x023] = "En_Holl (a room's edge, unseen)",
            [0x03B] = "En_River_Sound (a sound)", [0x03C] = "En_Horse_Normal (a horse)", [0x04C] = "En_Bombf (a bomb flower)",
            [0x059] = "Bg_Breakwall (a wall bombs break, before a way through)", [0x05A] = "En_Jj (Jabu-Jabu)", [0x060] = "En_Dekunuts (an enemy)",
            [0x094] = "Obj_Mure (fish, bugs)", [0x095] = "En_Sw (a gold skulltula)", [0x097] = "Object_Kankyo (an effect)", [0x09A] = "En_Horse_Link_Child (Epona)",
            [0x09B] = "Door_Ana (a grotto's hole)", [0x0A7] = "En_Encount1 (an enemy spawner)", [0x0B4] = "En_Encount2 (falling rocks)",
            [0x0BF] = "Bg_Spot17_Funen (the crater's smoke)", [0x0C7] = "En_Karebaba (an enemy)", [0x0D0] = "Bg_Bombwall (a wall bombs break)",
            [0x10F] = "Item_Etcetera (an item)", [0x112] = "En_Wonder_Item (unseen)", [0x115] = "En_Skj (an enemy)", [0x126] = "Obj_Bean (a bean's soil)",
            [0x12E] = "En_Okarina_Tag (unseen)", [0x130] = "En_Goroiwa (a rolling boulder)", [0x147] = "En_Wonder_Talk (unseen)",
            [0x165] = "En_Weather_Tag (unseen)", [0x173] = "Elf_Msg2 (unseen)", [0x183] = "Shot_Sun (unseen)", [0x185] = "En_Wonder_Talk2 (unseen)",
            [0x195] = "En_Shopnuts (an enemy)", [0x197] = "En_GeldB (an enemy)", [0x1A3] = "En_Dnt_Nomal (a scrub's game)",
            [0x1A8] = "Bg_Spot08_Bakudankabe (a wall bombs break)", [0x1A9] = "Bg_Spot17_Bakudankabe (a wall bombs break)", [0x1AF] = "En_Wf (an enemy)",
            [0x1CF] = "Obj_Makekinsuta (unseen)", [0x1D1] = "Obj_Timeblock (a block the Song of Time shows or hides)",
        };

        static Vector4 Col(int r, int g, int b, int al = 255) => new(r / 255f, g / 255f, b / 255f, al / 255f);
        static float Bin(float a) => a * MathF.PI * 2 / 65536f;
        static float SinS(float a) => MathF.Sin(Bin(a));
        static float CosS(float a) => MathF.Cos(Bin(a));

        // An actor's placement (Actor_Draw's: its position, raised by its shape's offset times its scale, its turn, its
        // scale), in the map.
        Matrix4x4 At(Vector3 pos, Vector3 rot, float s, float yOffset = 0) =>
            Matrix4x4.CreateScale(s) * Place(pos + new Vector3(0, yOffset * s, 0), rot) * partXf;

        // A display list drawn as the actor's draw does (after Gfx_SetupDL_25, with the colours it sets).
        void Draw(string dl, Matrix4x4 at, Vector4? prim = null, Vector4? env = null)
        {
            R.Xf = at;
            R.Setup25(prim, env);
            R.Run(dl);
        }

        // The actor's own collision (DynaPoly: its collision header, placed as the actor, scaled as it).
        void Collide(string path, Matrix4x4 at)
        {
            if (A.Has(path)) Coll.Add((path, at));
            else R.Missing++;
        }

        // A solid round what the game only bumps Link off (its collider cylinder: radius, height, from its base): an
        // upright prism of so many sides, capped, wound outward.
        int Solid(Vector3 basePos, float radius, float height, int sides = 0)
        {
            if (sides == 0) sides = radius >= 100 ? 8 : radius >= 40 ? 6 : 4;
            var c = Vector3.Transform(basePos, partXf);
            var lo = new Vector3[sides];
            var hi = new Vector3[sides];
            for (int i = 0; i < sides; i++)
            {
                float t = i * MathF.PI * 2 / sides;
                lo[i] = c + new Vector3(MathF.Cos(t) * radius, 0, MathF.Sin(t) * radius);
                hi[i] = lo[i] + new Vector3(0, height, 0);
            }
            int n0 = Solids.Count;
            void Tri(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 outward)
            {
                if (Vector3.Dot(Vector3.Cross(p1 - p0, p2 - p0), outward) < 0) (p1, p2) = (p2, p1);
                Solids.Add((p0, p1, p2));
            }
            for (int i = 0; i < sides; i++)
            {
                int j = (i + 1) % sides;
                var outward = (lo[i] + lo[j]) / 2 - c;
                Tri(lo[i], lo[j], hi[j], outward);
                Tri(lo[i], hi[j], hi[i], outward);
            }
            for (int i = 1; i + 1 < sides; i++) Tri(hi[0], hi[i], hi[i + 1], Vector3.UnitY);
            return Solids.Count - n0;
        }

        // The floor under a point (the scene's own collision: the highest floor at most so far above it), as OoT's floor
        // checks (BgCheck_EntityRaycastFloor) find it - null where there is none.
        float? Floor(Vector3 p, float above)
        {
            float top = p.Y + above;
            float? best = null;
            foreach (var (a, b, c) in floor)
            {
                if (MathF.Max(a.X, MathF.Max(b.X, c.X)) < p.X || MathF.Min(a.X, MathF.Min(b.X, c.X)) > p.X) continue;
                if (MathF.Max(a.Z, MathF.Max(b.Z, c.Z)) < p.Z || MathF.Min(a.Z, MathF.Min(b.Z, c.Z)) > p.Z) continue;
                float d = (b.Z - c.Z) * (a.X - c.X) + (c.X - b.X) * (a.Z - c.Z);
                if (MathF.Abs(d) < 1e-6f) continue;
                float w0 = ((b.Z - c.Z) * (p.X - c.X) + (c.X - b.X) * (p.Z - c.Z)) / d;
                float w1 = ((c.Z - a.Z) * (p.X - c.X) + (a.X - c.X) * (p.Z - c.Z)) / d;
                float w2 = 1 - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                float y = w0 * a.Y + w1 * b.Y + w2 * c.Y;
                if (y <= top && (best == null || y > best)) best = y;
            }
            return best;
        }

        // A turn of its own for what the game turns at random when its room gives it none (bushes, rocks): the same on
        // every run, from where it stands.
        static float Yaw(Vector3 p) => (uint)HashCode.Combine((int)p.X, (int)p.Y, (int)p.Z, 0x5EED) % 65536;

        void Count(string kind, int tris0, int solids0, int collTris)
        {
            var (n, t, c) = kinds.GetValueOrDefault(kind);
            kinds[kind] = (n + 1, t + R.Tris.Count - tris0, c + Solids.Count - solids0 + collTris);
        }

        public void Report()
        {
            Log("scenery (actors):");
            foreach (var (k, (n, t, c)) in kinds) Log($"  {k}: {n} placed, {t} triangles, {c} collision triangles");
            Log("  left out: " + string.Join(", ", skipped.Select(s => $"{s.Key} x{s.Value}")));
            Log($"  {Triangles} triangles drawn, {Solids.Count} solid triangles, {Coll.Count} actor collision meshes");
        }

        // The triangles of a collision header (to count what an actor's own collision adds).
        int CollTris(string path)
        {
            var d = A.Read(path);
            if (d == null) return 0;
            var r = new ResReader(d);
            for (int i = 0; i < 6; i++) r.S16();
            int nv = r.S32();
            r.Pos += nv * 6;
            return (int)r.U32();
        }

        // ---- a place's actors ----
        public void PlaceAll(string partName, string dir, List<string> rooms, Matrix4x4 xf)
        {
            part = partName;
            partXf = xf;
            floor = new();
            var collPath = A.Names.FirstOrDefault(n => n.StartsWith($"{dir}/{partName}_sceneCollisionHeader_"));
            if (collPath != null)
            {
                var r = new ResReader(A.Read(collPath));
                for (int i = 0; i < 6; i++) r.S16();
                int nv = r.S32();
                var v = new Vector3[nv];
                for (int i = 0; i < nv; i++) v[i] = new Vector3(r.S16(), r.S16(), r.S16());
                int np = (int)r.U32();
                for (int i = 0; i < np; i++)
                {
                    r.U16();
                    int ia = r.U16() & 0x1FFF, ib = r.U16() & 0x1FFF, ic = r.U16() & 0x1FFF;
                    var n = new Vector3(r.S16(), r.S16(), r.S16()) / 32767f;
                    r.S16();
                    if (ia < nv && ib < nv && ic < nv && n.Y > 0.3f) floor.Add((v[ia], v[ib], v[ic]));
                }
            }
            int tris0 = R.Tris.Count;
            var saveSeg = R.SegTextures;
            var saveState = R.State;
            foreach (var room in rooms)
                foreach (var (id, pos, rot, prm) in RoomActors(A, room))
                    One(id, pos, rot, prm);
            // the scene's doors (its transition actors: a door between two rooms or out of the scene)
            foreach (var (id, pos, rotY, prm) in TransitionActors(A.Read($"{dir}/{partName}_scene")))
                One(id, pos, new Vector3(0, rotY, 0), prm);
            R.SegTextures = saveSeg;
            R.State = saveState;
            Triangles += R.Tris.Count - tris0;
            R.Xf = Matrix4x4.Identity;
        }

        // The scene's transition actors (its command 0x0E: a count, then 16-byte entries - the rooms and cameras on each
        // side, the actor, its position, its turn about Y, its parameters).
        static List<(int id, Vector3 pos, float rotY, int prm)> TransitionActors(byte[] d)
        {
            var list = new List<(int, Vector3, float, int)>();
            if (d == null) return list;
            for (int at = 4; at + 8 < d.Length; at++)
            {
                if (BitConverter.ToUInt32(d, at) != 0x0E) continue;
                uint n = BitConverter.ToUInt32(d, at + 4);
                if (n == 0 || n > 64 || at + 8 + n * 16 > d.Length) continue;
                bool ok = true;
                for (int i = 0; i < n && ok; i++) ok = BitConverter.ToInt16(d, at + 8 + i * 16 + 4) is > 0 and < 0x1D7;
                if (!ok) continue;
                for (int i = 0; i < n; i++)
                {
                    int e = at + 8 + i * 16;
                    short S(int o) => BitConverter.ToInt16(d, e + o);
                    list.Add((S(4), new Vector3(S(6), S(8), S(10)), S(12), BitConverter.ToUInt16(d, e + 14)));
                }
                return list;
            }
            return list;
        }

        void One(int id, Vector3 pos, Vector3 rot, int prm)
        {
            int t0 = R.Tris.Count, s0 = Solids.Count, c0 = Coll.Count;
            if (!Actor(id, pos, rot, prm))
            {
                var name = Kept.GetValueOrDefault(id) ?? Left.GetValueOrDefault(id) ?? $"actor 0x{id:X3} (a person, an animal)";
                if (Kept.ContainsKey(id)) name += $" params {prm:X4}";
                skipped[name] = skipped.GetValueOrDefault(name) + 1;
                return;
            }
            int collTris = Coll.Skip(c0).Sum(c => CollTris(c.path));
            Count(Kept.GetValueOrDefault(id, $"0x{id:X3}"), t0, s0, collTris);
        }

        // One actor drawn (and made solid) as the game does - false for what is left out.
        bool Actor(int id, Vector3 pos, Vector3 rot, int prm)
        {
            switch (id)
            {
                case 0x009: return Door(pos, rot);
                case 0x026:  // En_Hata: a flagpole, its flag at rest (the skeleton's first frame), 1/75 and a tenth taller
                {
                    var at = Matrix4x4.CreateScale(1, 1.1f, 1) * At(pos, rot, 1 / 75f);
                    R.Setup25();
                    Skeleton("objects/object_hata/gFlagpoleSkel", "objects/object_hata/gFlagpoleFlapAnim", at);
                    Collide("objects/object_hata/gFlagpoleCol", At(pos, rot, 1 / 75f));
                    return true;
                }
                case 0x039:  // En_A_Obj: in rooms only its signposts (9 oblong, 10 an arrow), 0.01
                {
                    int type = prm & 0xFF;
                    if (type is not (9 or 10)) return false;
                    Draw(type == 9 ? "objects/gameplay_keep/gSignRectangularDL" : "objects/gameplay_keep/gSignDirectionalDL", At(pos, rot, 0.01f));
                    Solid(pos, 25, 60, 4);
                    return true;
                }
                case 0x03E:  // Bg_Treemouth: the Deku Tree's mouth, shut (its environment colour's alpha: 500 * 0.1)
                    Draw("objects/object_spot04_objects/gDekuTreeMouthDL", At(pos, rot, 1), env: Col(128, 128, 128, 50));
                    Collide("objects/object_spot04_objects/gDekuTreeMouthCol", At(pos, rot, 1));
                    return true;
                case 0x04A:  // Bg_Spot00_Hanebasi: the drawbridge itself (-1; down, as by day) and its chains
                {
                    if (prm != 0xFFFF) return false;
                    const string obj = "objects/object_spot00_objects/";
                    var bridge = Place(pos, new Vector3(0, rot.Y, 0)) * partXf;
                    Draw(obj + "gHyruleFieldCastleDrawbridgeDL", bridge);
                    Collide(obj + "gHyruleFieldCastleDrawbridgeCol", bridge);
                    // its chains, at the bridge's corners (as its draw puts them), turned as it is
                    foreach (float side in new[] { 158f, -158f })
                    {
                        var at = Vector3.Transform(new Vector3(side, 10, 400), Place(pos, new Vector3(0, rot.Y, 0)));
                        var chain = Place(at, new Vector3(0, rot.Y, 0)) * partXf;
                        Draw(obj + "gHyruleFieldCastleDrawbridgeChainsDL", chain);
                        Collide(obj + "gHyruleFieldCastleDrawbridgeChainsCol", chain);
                    }
                    return true;
                }
                case 0x05E:  // Obj_Syokudai: a torch (its kind the parameters' top four bits: golden, timed, wooden), unlit
                {
                    string[] dl = { "gGoldenTorchDL", "gTimedTorchDL", "gWoodenTorchDL" };
                    Draw("objects/object_syokudai/" + dl[Math.Min(2, prm >> 12)], At(pos, rot, 1));
                    Solid(pos, 12, 45);
                    return true;
                }
                case 0x06E:  // Bg_Mjin: a warp pad (0 the ocarina's, else a medallion's: its emblem in segment 8)
                {
                    if (prm == 0)
                    {
                        Draw("objects/object_mjin_oka/gOcarinaWarpPadDL", At(pos, rot, 1));
                        Collide("objects/object_mjin_oka/gOcarinaWarpPadCol", At(pos, rot, 1));
                        return true;
                    }
                    string[] emblems = { "object_mjin_flash/gLightMedallionPlatformTex", "object_mjin_dark/gShadowMedallionPlatformTex",
                                         "object_mjin_flame/gFireMedallionPlatformTex", "object_mjin_ice/gWaterMedallionPlatformTex",
                                         "object_mjin_soul/gSpiritMedallionPlatformTex", "object_mjin_wind/gForestMedallionPlatformTex" };
                    if (prm > emblems.Length) return false;
                    R.SegTextures = new() { [8] = "objects/" + emblems[prm - 1] };
                    Draw("objects/object_mjin/gWarpPadBaseDL", At(pos, rot, 1));
                    R.SegTextures = SceneSegmentTextures.GetValueOrDefault(part);
                    Collide("objects/object_mjin/gWarpPadCol", At(pos, rot, 1));
                    return true;
                }
                case 0x077: return Wood(pos, rot, prm);
                case 0x09C:  // Bg_Spot02_Objects: the graveyard's royal tomb's stone (0), its hole's cover (1), the tomb (2);
                {            // 3 and 4 its lightning and the sun's song's light, left out
                    int type = prm & 0xFF;
                    var parts = type switch
                    {
                        0 => ("object_spot02_objects_DL_012A50", "object_spot02_objects_Col_012BA4"),
                        1 => ("object_spot02_objects_DL_0127C0", "object_spot02_objects_Col_0128D8"),
                        2 => ("object_spot02_objects_DL_0130B0", "object_spot02_objects_Col_0133EC"),
                        _ => (null, null),
                    };
                    if (parts.Item1 == null) return false;
                    Draw("objects/object_spot02_objects/" + parts.Item1, At(pos, rot, 0.1f));
                    Collide("objects/object_spot02_objects/" + parts.Item2, At(pos, rot, 0.1f));
                    return true;
                }
                case 0x09D:  // Bg_Haka: a gravestone, and the earth before it (unmoved)
                    Draw("objects/object_haka/gGravestoneStoneDL", At(pos, rot, 0.1f));
                    Draw("objects/object_haka/gGravestoneEarthDL", At(pos, rot, 0.1f));
                    Collide("objects/object_haka/gGravestoneCol", At(pos, rot, 0.1f));
                    return true;
                case 0x0B8:  // Bg_Spot09_Obj, Gerudo Valley's: 0 the bridge's sides, 4 the bridge as the carpenters mend it
                {            // (adult Link's: whole), 3 their tent (at 0.1) - each its model and collision (sDLists, D_808B1F90)
                    var parts9 = (prm & 0xFF) switch
                    {
                        0 => new[] { ("gValleyBridgeSidesDL", (string)null) },
                        3 => new[] { ("gCarpentersTentDL", "gValleyObjects3Col"), ("gCarpentersTentEntranceDL", null) },
                        4 => new[] { ("gValleyRepairedBridgeDL", "gValleyObjects4Col") },
                        _ => null,  // (1 the broken bridge, 2 the child's: not adult Link's)
                    };
                    if (parts9 == null) return false;
                    var at9 = At(pos, new Vector3(0, rot.Y, 0), (prm & 0xFF) == 3 ? 0.1f : 1f);
                    foreach (var (dl, col) in parts9)
                    {
                        Draw("objects/object_spot09_obj/" + dl, at9);
                        if (col != null) Collide("objects/object_spot09_obj/" + col, at9);
                    }
                    return true;
                }
                case 0x0B9:  // Bg_Spot18_Obj: Goron City's statue (0) and its spear (1), 0.1
                {
                    int type = prm & 0xF;
                    if (type > 1) return false;
                    Draw("objects/object_spot18_obj/" + (type == 0 ? "gGoronCityStatueDL" : "gGoronCityStatueSpearDL"), At(pos, rot, 0.1f));
                    Collide("objects/object_spot18_obj/" + (type == 0 ? "gGoronCityStatueCol" : "gGoronCityStatueSpearCol"), At(pos, rot, 0.1f));
                    return true;
                }
                case 0x0CD:  // Bg_Spot16_Bombstone: the boulder before Dodongo's Cavern (0xFF; the rest its pieces), 0.4,
                {            // its cylinder 190 wide and 80 tall, set off (10, 0, 50)
                    if ((prm & 0xFF) != 0xFF) return false;
                    Draw("objects/object_spot16_obj/gDodongosCavernRock3DL", At(pos, rot, 0.4f));
                    Solid(pos + new Vector3(10, 0, 50), 190, 80);
                    return true;
                }
                case 0x0D1:  // Bg_Spot08_Iceblock: Zora's Fountain's ice (adult Link's), a ramp or a floe, its size by 0xF0
                {
                    float s = (prm & 0xF0) switch { 0 => 0.2f, 0x10 => 0.1f, _ => 0.05f };
                    bool ramp = (prm & 0x200) != 0;
                    Draw("objects/object_spot08_obj/" + (ramp ? "gZorasFountainIceRampDL" : "gZorasFountainIcebergDL"), At(pos, rot, s));
                    Collide("objects/object_spot08_obj/" + (ramp ? "gZorasFountainIceRampCol" : "gZorasFountainIcebergCol"), At(pos, rot, s));
                    return true;
                }
                case 0x0D5:  // Bg_Spot06_Objects (its kind the parameters' high byte): Lake Hylia's water, full (1.0, by
                {            // day as when the Water Temple is done), the Water Temple's gate and its lock (0.1)
                    switch ((prm >> 8) & 0xFF)
                    {
                        case 0:
                            Draw("objects/object_spot06_objects/gLakeHyliaWaterTempleGateDL", At(pos, rot, 0.1f));
                            Collide("objects/object_spot06_objects/gLakeHyliaWaterTempleGateCol", At(pos, rot, 0.1f));
                            return true;
                        case 1:
                            Draw("objects/object_spot06_objects/gLakeHyliaWaterTempleKeyDL", At(pos, rot, 0.1f));
                            return true;
                        case 2:
                            Draw("objects/object_spot06_objects/gLakeHyliaHighWaterDL", At(pos, rot, 1), env: Col(255, 255, 255, 128));
                            return true;
                        default: return false;
                    }
                }
                case 0x0E5:  // Bg_Spot16_Doughnut: the cloud ring round Death Mountain's top (Kakariko's seen from below, 0.04)
                {
                    if (prm != 0xFFFF || part == "spot01") return false;  // (Kakariko's, seen from below the mountain: not here, as the mountain is not)
                    Draw("objects/object_efc_doughnut/gDeathMountainCloudCircleNormalDL", At(pos, rot, 0.1f), env: Col(255, 255, 255));
                    return true;
                }
                case 0x0F8:  // Bg_Spot15_Saku: the castle's fence, 0.1
                    Draw("objects/object_spot15_obj/gLonLonCorralFenceDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot15_obj/gLonLonCorralFenceCol", At(pos, rot, 0.1f));
                    return true;
                case 0x100:  // Bg_Gate_Shutter: Kakariko's gate to Death Mountain, 1.0 - open, as once the guard lets Link by
                {
                    var p = part == "spot01" ? new Vector3(-89, pos.Y, -1375) : pos;
                    Draw("objects/object_spot01_matoyab/gKakarikoGuardGateDL", At(p, rot, 1));
                    Collide("objects/object_spot01_matoyab/gKakarikoGuardGateCol", At(p, rot, 1));
                    return true;
                }
                case 0x102:  // Bg_Spot01_Fusya: the windmill's sails, 0.1 (turning in the game: here at rest)
                    Draw("objects/object_spot01_objects/gKakarikoWindmillSailsDL", At(pos, rot, 0.1f));
                    return true;
                case 0x103:  // Bg_Spot01_Idohashira: the beam over the well, 0.1
                    Draw("objects/object_spot01_objects/gKakarikoWellArchDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot01_objects/gKakarikoWellArchCol", At(pos, rot, 0.1f));
                    return true;
                case 0x104:  // Bg_Spot01_Idomizu: the well's water, 0.1
                    Draw("objects/object_spot01_objects/gKakarikoWellWaterDL", At(pos, rot, 0.1f));
                    return true;
                case 0x107:  // Bg_Spot15_Rrbox: a milk crate, 0.1
                    Draw("objects/object_spot15_obj/gLonLonMilkCrateDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot15_obj/gLonLonMilkCrateCol", At(pos, rot, 0.1f));
                    return true;
                case 0x108:  // Bg_Umajump: a fence a horse jumps, 0.1
                    Draw("objects/object_umajump/gJumpableHorseFenceDL", At(pos, rot, 0.1f));
                    Collide("objects/object_umajump/gJumpableHorseFenceCol", At(pos, rot, 0.1f));
                    return true;
                case 0x111:  // Obj_Tsubo: a pot (bit 8: the dungeons' or its own object's), 0.15
                    Draw((prm >> 8 & 1) == 0 ? "objects/gameplay_dangeon_keep/gPotDL" : "objects/object_tsubo/object_tsubo_DL_0017C0", At(pos, rot, 0.15f));
                    return true;
                case 0x11F:  // Bg_Spot03_Taki: Zora's River's waterfall (shut), 0.1 - its middle's vertices loaded by its
                {            // draw; not solid, the way to Zora's Domain being behind it
                    var at = At(pos, rot, 0.1f);
                    const string o = "objects/object_spot03_object/";
                    Draw(o + "object_spot03_object_DL_000B20", at);
                    R.LoadVertices(o + "object_spot03_object_Vtx_000800", 25, 0);
                    R.Run(o + "object_spot03_object_DL_000BC0");
                    R.Run(o + "object_spot03_object_DL_001580");
                    return true;
                }
                case 0x120:  // Bg_Spot07_Taki: Zora's Domain's waterfalls, flowing (the child's: the adult's frozen, as the
                {            // scene's water is not), 0.1
                    Draw("objects/object_spot07_object/" + (prm == 0 ? "object_spot07_object_DL_000460" : "object_spot07_object_DL_000BE0"), At(pos, rot, 0.1f), env: Col(255, 255, 255, 128));
                    return true;
                }
                case 0x125: return Kusa(pos, rot, prm);
                case 0x127:  // Obj_Bombiwa: a boulder bombs break, 0.1 (raised 20, its shape lowered 200 * 0.1: where it is)
                {
                    var r = rot.Y == 0 ? new Vector3(rot.X, Yaw(pos), rot.Z) : rot;
                    Draw("objects/object_bombiwa/object_bombiwa_DL_0009E0", At(pos + new Vector3(0, 20, 0), r, 0.1f, -200));
                    Solid(pos, 55, 70);
                    return true;
                }
                case 0x12D:  // Obj_Hsblock: a hookshot post (0, 1) or target (2), 0.1
                {
                    int type = prm & 3;
                    if (type > 2) return false;
                    Draw("objects/object_d_hsblock/" + (type == 2 ? "gHookshotTargetDL" : "gHookshotPostDL"), At(pos, rot, 0.1f));
                    Collide("objects/object_d_hsblock/" + (type == 2 ? "gHookshotTargetCol" : "gHookshotPostCol"), At(pos, rot, 0.1f));
                    return true;
                }
                case 0x141:  // En_Kanban: a sign (whole), 0.01, set back 100 of its units
                {
                    var at = Matrix4x4.CreateTranslation(0, 0, -100) * At(pos, rot, 0.01f);
                    Draw("objects/object_kanban/object_kanban_DL_000C30", at);
                    R.Run("objects/gameplay_keep/gSignRectangularDL");
                    Solid(pos, 20, 50, 4);
                    return true;
                }
                case 0x14E: return Ishi(pos, rot, prm, true);
                case 0x14F:  // Obj_Hana: a flower (0), a small rock (1), a bush (2), each at its scale and height
                {
                    var (dl, s, y) = (prm & 3) switch
                    {
                        0 => ("objects/gameplay_field_keep/gHanaDL", 0.01f, 0f),
                        1 => ("objects/gameplay_field_keep/gFieldKakeraDL", 0.1f, 58f),
                        _ => ("objects/gameplay_field_keep/gFieldBushDL", 0.4f, 0f),
                    };
                    Draw(dl, At(pos, rot, s, y));
                    return true;
                }
                case 0x151:  // Obj_Mure2: a ring of bushes (0: one in the middle, eight round it 80 away), bushes scattered
                {            // (1), a ring of rocks (2: eight) - each spawned as the game spawns it, turned at random
                    int type = prm & 3;
                    if (type > 2) return false;
                    var at = new List<Vector3>();
                    if (type == 0)
                    {
                        at.Add(pos);
                        for (int i = 1; i < 9; i++) at.Add(pos + new Vector3(80 * SinS((i - 1) * 0x2000), 0, 80 * CosS((i - 1) * 0x2000)));
                    }
                    else if (type == 1)
                    {
                        (float r, int a)[] scattered = { (40, 0x0666), (40, 0x2CCC), (40, 0x5999), (40, 0x8666), (20, 0xC000), (80, 0x1333),
                                                         (80, 0x4000), (80, 0x6CCC), (80, 0x9333), (80, 0xACCC), (80, 0xC666), (60, 0xE000) };
                        foreach (var (r, a) in scattered) at.Add(pos + new Vector3(r * CosS(a), 0, -r * SinS(a)));
                    }
                    else for (int i = 0; i < 8; i++) at.Add(pos + new Vector3(80 * SinS(i * 0x2000), 0, 80 * CosS(i * 0x2000)));
                    foreach (var p in at)
                    {
                        if (type == 2) Ishi(p, new Vector3(rot.X, 0, rot.Z), 0, true);
                        else Kusa(p, new Vector3(rot.X, 0, rot.Z), 0);
                    }
                    return true;
                }
                case 0x15C:  // Bg_Spot18_Basket: Goron City's great urn, 0.1
                    Draw("objects/object_spot18_obj/gGoronCityVaseDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot18_obj/gGoronCityVaseCol", At(pos, rot, 0.1f));
                    return true;
                case 0x172:  // Door_Gerudo: a cell door in the hideout, 1.0 (a door: not solid)
                    Draw("objects/object_door_gerudo/gGerudoCellDoorDL", At(pos, rot, 1));
                    return true;
                case 0x18D:  // Bg_Spot05_Soko: the Sacred Forest Meadow's wall (0) and grate (1), 0.1
                {
                    int type = prm & 0xFF;
                    if (type > 1) return false;
                    Draw("objects/object_spot05_objects/" + (type == 0 ? "object_spot05_objects_DL_000840" : "object_spot05_objects_DL_001190"), At(pos, rot, 0.1f));
                    Collide("objects/object_spot05_objects/" + (type == 0 ? "object_spot05_objects_Col_000918" : "object_spot05_objects_Col_0012C0"), At(pos, rot, 0.1f));
                    return true;
                }
                case 0x190:  // Bg_Spot12_Gate: the Gerudos' Fortress's gate to the desert, 0.1
                    Draw("objects/object_spot12_obj/gGerudoFortressWastelandGateDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot12_obj/gGerudoFortressWastelandGateCol", At(pos, rot, 0.1f));
                    return true;
                case 0x191:  // Bg_Spot12_Saku: the shutter before the Gerudo Training Ground, 0.1
                    Draw("objects/object_spot12_obj/gGerudoFortressGTGShutterDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot12_obj/gGerudoFortressGTGShutterCol", At(pos, rot, 0.1f));
                    return true;
                case 0x19D:  // Bg_Spot01_Objects2: Kakariko's shop signs (0-2), building site (3), shooting gallery (4), 0.1
                {
                    int type = prm & 7;
                    string[] dl = { "object_spot01_matoya/gKakarikoPotionShopSignDL", "object_spot01_matoya/gKakarikoShootingGallerySignDL",
                                    "object_spot01_matoya/gKakarikoBazaarSignDL", "object_spot01_matoyab/gKakarikoConstructionSiteDL",
                                    "object_spot01_matoya/gKakarikoShootingGalleryDL" };
                    if (type > 4) return false;
                    Draw("objects/" + dl[type], At(pos, rot, 0.1f));
                    if (type == 3) Collide("objects/object_spot01_matoyab/object_spot01_matoyab_col", At(pos, rot, 0.1f));
                    if (type == 4) Collide("objects/object_spot01_matoya/gKakarikoShootingGalleryCol", At(pos, rot, 0.1f));
                    return true;
                }
                case 0x19E:  // Obj_Comb: a beehive, 0.1, turned about a point 118 of its units up
                {
                    float s = 0.1f;
                    var at = Matrix4x4.CreateScale(s) * Matrix4x4.CreateTranslation(0, -118 * s, 0) * Place(pos + new Vector3(0, 118 * s, 0), rot) * partXf;
                    Draw("objects/gameplay_field_keep/gFieldBeehiveDL", at);
                    return true;
                }
                case 0x1A0:  // Obj_Kibako2: a large crate, 0.1
                    Draw("objects/object_kibako2/gLargeCrateDL", At(pos, rot, 0.1f));
                    Collide("objects/object_kibako2/gLargeCrateCol", At(pos, rot, 0.1f));
                    return true;
                case 0x1B9:  // En_Gs: a gossip stone, 0.1, its colour white
                {
                    var at = At(pos, rot, 0.1f);
                    Draw("objects/object_gs/gGossipStoneMaterialDL", at, prim: Vector4.One);
                    R.Run("objects/object_gs/gGossipStoneDL");
                    R.Run("objects/object_gs/gGossipStoneSquishedDL");
                    Solid(pos, 21, 48);
                    return true;
                }
                case 0x1C4:  // Bg_Spot18_Shutter: a door of Goron City (shut), 0.1
                    Draw("objects/object_spot18_obj/gGoronCityDoorDL", At(pos, rot, 0.1f));
                    Collide("objects/object_spot18_obj/gGoronCityDoorCol", At(pos, rot, 0.1f));
                    return true;
                case 0x1D2:  // Obj_Hamishi: a bronze boulder (the silver rock's model, coloured), 0.4, its shape raised 80
                {
                    var r = rot.Y == 0 ? new Vector3(rot.X, Yaw(pos), rot.Z) : rot;
                    Draw("objects/gameplay_field_keep/gSilverRockDL", At(pos, r, 0.4f, 80), prim: Col(255, 170, 130));
                    Solid(pos, 50, 70);
                    return true;
                }
            }
            return false;
        }

        // ---- En_Wood02: trees and bushes (z_en_wood02.c) ----
        // Its kind the parameters' low byte. A spawner stands for a group: five more round it (sSpawnDistance, sSpawnAngle),
        // spawned as Link nears, itself set 141 off; each of the group set on the floor.
        static readonly float[] SpawnDistance = { 707, 525, 510, 500, 566, 141 };
        static readonly int[] SpawnAngle = { 0x1FFF, 0x4C9E, 0x77F5, 0xA5C9, 0xD6C3, 0xA000 };

        bool Wood(Vector3 pos, Vector3 rot, int prm)
        {
            int type = prm & 0xFF;
            if (type > 0x16) return false;  // (falling leaves)
            bool spawner = type is 0x03 or 0x06 or 0x08 or 0x0D or 0x0F or 0x13 or 0x15;
            bool spawned = type is 0x04 or 0x07 or 0x09 or 0x0E or 0x10 or 0x14 or 0x16;
            if (spawner)
            {
                int extra = type == 0x0F ? 0x4000 : 0;
                for (int i = 0; i < 5; i++)
                {
                    float a = SpawnAngle[i] + rot.Y + extra;
                    var p = pos + new Vector3(SpawnDistance[i] * SinS(a), 0, SpawnDistance[i] * CosS(a));
                    var y = Floor(p, 200);
                    if (y == null) continue;  // (over the void: the game removes it)
                    OneWood(type + 1, new Vector3(p.X, y.Value, p.Z), new Vector3(rot.X, SpawnAngle[i], 0));
                }
                float a5 = SpawnAngle[5] + rot.Y + extra;
                pos += new Vector3(SpawnDistance[5] * SinS(a5), 0, SpawnDistance[5] * CosS(a5));
            }
            if (spawner || spawned)
            {
                var y = Floor(pos, 200);
                if (y == null) return true;
                pos.Y = y.Value;
            }
            OneWood(type, pos, new Vector3(rot.X, rot.Y, 0));
            return true;
        }

        void OneWood(int type, Vector3 pos, Vector3 rot)
        {
            float s = type switch { 0x00 or 0x0C or 0x0F or 0x10 or 0x12 or 0x15 or 0x16 => 1.5f, 0x02 => 0.6f, _ => 1f };
            int draw = type <= 0x04 ? 0 : type <= 0x09 ? 1 : type == 0x0A ? 2 : type <= 0x10 ? 3 : 4;
            var colour = type is 0x05 or 0x08 or 0x09 ? Col(50, 170, 70, 0) : type is 0x06 or 0x07 ? Col(180, 155, 0, 0) : Col(255, 255, 255, 0);
            const string o = "objects/object_wood02/";
            string[] opa = { "object_wood02_DL_0078D0", "object_wood02_DL_007CA0", "object_wood02_DL_0080D0", "object_wood02_DL_000090", "object_wood02_DL_000340" };
            string[] xlu = { "object_wood02_DL_007968", "object_wood02_DL_007D38", "object_wood02_DL_0081A8", null, null };
            var at = At(pos, rot, s);
            // a tree: its trunk, then its leaves (their colour the environment's); a bush: the one list
            Draw(o + opa[draw], at, env: colour);
            if (xlu[draw] != null) Draw(o + xlu[draw], at, env: colour);
            // the trees' trunks solid (the bushes the game lets Link through): its cylinder 18 wide, as tall as the trunk
            if (type <= 0x0A)
            {
                float trunk = draw switch { 0 => 400, 1 => 307, _ => 277 };
                Solid(pos, 18 * s, trunk * s, 5);
            }
        }

        // ---- En_Kusa: a bush (0) or grass (1, 2), 0.4, turned at random when its room gives no turn, set on the floor ----
        bool Kusa(Vector3 pos, Vector3 rot, int prm)
        {
            int type = prm & 3;
            if (type > 2) return false;
            if (rot.Y == 0) rot.Y = Yaw(pos);
            if (Floor(pos, 30) is float y && y > pos.Y - 300) pos.Y = y;
            Draw(type == 0 ? "objects/gameplay_field_keep/gFieldBushDL" : "objects/object_kusa/object_kusa_DL_000140", At(pos, rot, 0.4f));
            return true;
        }

        // ---- En_Ishi: a small rock (0: 0.1, its shape raised 58) or a large silver one (1: 0.4, raised 80, solid) ----
        bool Ishi(Vector3 pos, Vector3 rot, int prm, bool snap)
        {
            int type = prm & 1;
            if (rot.Y == 0) rot.Y = Yaw(pos);
            if (snap && (prm >> 5 & 1) == 0 && Floor(pos, 30) is float y && y > pos.Y - 300) pos.Y = y;
            if (type == 0) Draw("objects/gameplay_field_keep/gFieldKakeraDL", At(pos, rot, 0.1f, 58));
            else
            {
                Draw("objects/gameplay_field_keep/gSilverRockDL", At(pos, rot, 0.4f, 80), prim: Vector4.One);
                Solid(pos, 55, 70);
            }
            return true;
        }

        // ---- En_Door: a door (0.01), its skeleton at rest - its panel the field's door (OBJECT_GAMEPLAY_FIELD_KEEP's,
        // as every scene but the temples'); a door, not solid ----
        bool Door(Vector3 pos, Vector3 rot)
        {
            R.Setup25();
            Skeleton("objects/gameplay_keep/gDoorSkel", "objects/gameplay_keep/gDoorAdultOpeningLeftAnim", At(pos, new Vector3(0, rot.Y, 0), 0.01f),
                     (limb, dl) => limb == 4 ? "objects/gameplay_field_keep/gFieldDoorLeftDL" : dl);
            return true;
        }

        // A skeleton (SkelAnime's, a normal one) drawn in its animation's first frame: each limb turned by its joint
        // (Z, then Y, then X, about where it is joined: Matrix_TranslateRotateZYX), its children after it, its siblings
        // from its parent. limbDl: a limb's list as an actor's OverrideLimbDraw changes it (by its 1-based number).
        void Skeleton(string skel, string anim, Matrix4x4 at, Func<int, string, string> limbDl = null)
        {
            var sd = A.Read(skel);
            var ad = A.Read(anim);
            if (sd == null || ad == null) { R.Missing++; return; }
            var r = new ResReader(sd);
            int skelType = r.S8(); r.S8();
            r.U32(); r.U32(); r.S8();
            int count = (int)r.U32();
            if (skelType != 0) return;  // (flex and curve skeletons: not drawn)
            var limbs = new List<(Vector3 trans, int child, int sibling, string dl)>();
            for (int i = 0; i < count; i++)
            {
                var ld = A.Read(r.Str());
                if (ld == null) { R.Missing++; return; }
                var l = new ResReader(ld);
                l.S8(); l.S8(); l.Str(); l.U16();
                if (l.U32() != 0) return;  // (a skinned limb)
                l.Str();
                l.Pos += 12 + 6;           // (its leg's translation, its rest turn: unused by a normal skeleton)
                l.Str(); l.Str();
                string dl = l.Str();
                l.Str();
                var trans = new Vector3(l.S16(), l.S16(), l.S16());
                limbs.Add((trans, l.U8(), l.U8(), dl));
            }
            // the animation's first frame: a value each for the root's position, then each limb's turn (x, y, z)
            var ar = new ResReader(ad);
            if (ar.U32() != 0) return;
            ar.S16();
            var values = new ushort[ar.U32()];
            for (int i = 0; i < values.Length; i++) values[i] = ar.U16();
            int nj = (int)ar.U32();
            var joints = new Vector3[nj];
            short V(int i) => (short)(i < values.Length ? values[i] : 0);
            for (int i = 0; i < nj; i++) joints[i] = new Vector3(V(ar.U16()), V(ar.U16()), V(ar.U16()));
            Vector3 Joint(int i) => i < joints.Length ? joints[i] : Vector3.Zero;
            void Limb(int i, Matrix4x4 parent, int depth)
            {
                if (i >= limbs.Count || depth > 64) return;
                var (trans, child, sibling, dl) = limbs[i];
                var p = i == 0 ? Joint(0) : trans;
                var t = Joint(i + 1);
                var m = Matrix4x4.CreateRotationX(Bin(t.X)) * Matrix4x4.CreateRotationY(Bin(t.Y)) * Matrix4x4.CreateRotationZ(Bin(t.Z)) * Matrix4x4.CreateTranslation(p) * parent;
                if (limbDl != null) dl = limbDl(i + 1, dl);
                if (!string.IsNullOrEmpty(dl)) { R.Xf = m; R.Run(dl); }
                if (child != 0xFF) Limb(child, m, depth + 1);
                if (sibling != 0xFF) Limb(sibling, parent, depth + 1);
            }
            Limb(0, at, 0);
        }
    }
}
