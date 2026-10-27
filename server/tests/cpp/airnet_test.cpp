// airnet_test: the game's online client (src/runtime/airnet.cpp) against a real master server.
//
//   airnet_test --master 127.0.0.1:27600 [--prefix cpp] [--udp-blocked 1] [--maps 1|2]
//
// Two clients register, one hosts, the other finds the server and joins, both enter a match group,
// and LAN frames go through the relay both ways - over UDP, then with one and both clients on the TCP
// relay. With --udp-blocked 1 (a server whose UDP never answers) the clients must fall back to TCP by
// themselves. Also checks the crypto against known vectors. With --maps 1 (2): the host's City Trial map
// downloaded by the consoles without it (peer to peer, and through the relay over TCP - UDP), instead.
// Run by server/tests/test_cpp_client.py (which starts the server); exit code 0 = pass.
#include "runtime/airnet.h"
#include "runtime/mapfiles.h"
#include "runtime/settings.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// ---- what the runtime would provide ----
namespace rt {
void vlog(const char* fmt, va_list ap) {
    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);
}
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
namespace lan {
static Port* g_port;
void setPort(Port* p) { g_port = p; }
Port* port() { return g_port; }
void setLocalDhcp(bool) {}
bool linkWanted() { return false; }
void macAddress(uint8_t out[6]) {
    const uint8_t m[6] = {0x02, 0x4B, 0x41, 0x52, 0x00, 0x01};
    std::memcpy(out, m, 6);
}
}  // namespace lan
}  // namespace rt

using namespace rt::airnet;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// Waits until `pred(snapshot)` holds (or times out, printing the client's last error).
static bool waitFor(Client& c, const char* what, const std::function<bool(const Snapshot&)>& pred, int ms = 15000) {
    Snapshot s;
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        c.snapshot(s);
        if (pred(s)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    c.snapshot(s);
    std::printf("FAILED waiting for %s (state %s, error \"%s\")\n", what, stateName(s.state), s.error.c_str());
    g_failures++;
    return false;
}

static bool receive(rt::lan::Port& port, std::vector<uint8_t>& frame, int ms = 3000) {
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (port.poll(frame)) return true;
        std::this_thread::yield();
    }
    return false;
}

static std::vector<uint8_t> ethernet(const uint8_t dst[6], const uint8_t src[6], const std::string& payload) {
    std::vector<uint8_t> f(dst, dst + 6);
    f.insert(f.end(), src, src + 6);
    f.push_back(0x88);
    f.push_back(0xB5);
    f.insert(f.end(), payload.begin(), payload.end());
    while (f.size() < 60) f.push_back(0);
    return f;
}

// Probes until a frame gets from A to B and back (after a path or transport changes, the first frames
// may be lost, as on any network), then empties both ports.
static void settle(Client& a, Client& b, const uint8_t macA[6], const uint8_t macB[6]) {
    std::vector<uint8_t> got;
    auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        auto probe = ethernet(macB, macA, "probe");
        a.port().send(probe.data(), probe.size());
        if (!receive(b.port(), got, 200)) continue;
        auto back = ethernet(macA, macB, "probe back");
        b.port().send(back.data(), back.size());
        if (!receive(a.port(), got, 200)) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (a.port().poll(got) || b.port().poll(got)) {
        }
        return;
    }
    CHECK(!"the path settles");
}

// Round trips A -> B -> A through the relay; prints the median and p95.
static void roundTrips(Client& a, Client& b, const uint8_t macA[6], const uint8_t macB[6], const char* label) {
    settle(a, b, macA, macB);
    std::vector<uint8_t> got;
    std::vector<long long> rtts;
    for (int i = 0; i < 200; i++) {
        auto ping = ethernet(macB, macA, "ping " + std::to_string(i));
        auto t0 = Clock::now();
        a.port().send(ping.data(), ping.size());
        if (!receive(b.port(), got, 1000)) break;
        auto pong = ethernet(macA, macB, "pong " + std::to_string(i));
        b.port().send(pong.data(), pong.size());
        if (!receive(a.port(), got, 1000) || got != pong) break;
        rtts.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    }
    CHECK(rtts.size() == 200);
    if (!rtts.empty()) {
        std::sort(rtts.begin(), rtts.end());
        std::printf("relay round trip A->B->A, %s: median %lld us, p95 %lld us\n", label, rtts[rtts.size() / 2],
                    rtts[rtts.size() * 95 / 100]);
    }
}

static void cryptoVectors() {
    uint8_t out[64];
    detail::sha256("abc", 3, out);
    CHECK(hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // RFC 4231 test case 2
    detail::hmacSha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    CHECK(hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 7914 section 12
    CHECK(detail::scrypt("", 0, "", 0, 16, 1, 1, out, 64));
    CHECK(hex(out, 64) ==
          "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
          "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
    // RFC 7914's second vector uses p=16, past what a server may ask of a client: refused.
    CHECK(!detail::scrypt("password", 8, "NaCl", 4, 1024, 8, 16, out, 64));
    auto t0 = Clock::now();
    CHECK(detail::scrypt("pleaseletmein", 13, "SodiumChloride", 14, 16384, 8, 1, out, 64));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(hex(out, 64) ==
          "7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2"
          "d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887");
    std::printf("crypto vectors ok (scrypt N=16384 r=8: %lld ms)\n", static_cast<long long>(ms));
}

static const Member* memberNamed(const Snapshot& s, const std::string& name) {
    for (auto& m : s.lobby.members)
        if (m.name == name) return &m;
    return nullptr;
}

static Path pathTo(Client& c, const std::string& name) {
    const Member* m = memberNamed(c.snapshot(), name);
    return m ? m->path : Path::None;
}

// Frames that arrive at `port` within `ms` (to see that one came exactly once).
static int countArrivals(rt::lan::Port& port, const std::vector<uint8_t>& want, int ms) {
    std::vector<uint8_t> got;
    int n = 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (port.poll(got)) {
            if (got == want) n++;
        } else {
            std::this_thread::yield();
        }
    }
    return n;
}

static void registerAll(std::vector<Client*> cs, const std::vector<std::string>& names, const std::string& password) {
    for (size_t i = 0; i < cs.size(); i++) cs[i]->registerAccount(names[i], password, false);
    for (size_t i = 0; i < cs.size(); i++)
        waitFor(*cs[i], "online", [](const Snapshot& s) { return s.state == State::Online && s.loggedIn; });
}

// Three consoles behind simulated NATs (A and B: port-restricted cone, C: symmetric), a master whose
// UDP gets nowhere (its relay runs over TCP, as on the VPS): A and B find a direct path by hole
// punching; C cannot, and reaches both through the relay - in one match.
static int natTest([[maybe_unused]] const std::string& master, const std::string& prefix, const std::string (&nat)[3]) {
    const std::string password = "correct horse";
    const std::string names[3] = {prefix + "_ann", prefix + "_ben", prefix + "_cat"};
    const uint8_t macs[3][6] = {{2, 0x4B, 0x41, 0x52, 1, 0xA}, {2, 0x4B, 0x41, 0x52, 1, 0xB}, {2, 0x4B, 0x41, 0x52, 1, 0xC}};
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Client a, b, c;
    Client* cs[3] = {&a, &b, &c};
    for (int i = 0; i < 3; i++) {
        detail::setUdpGateway(*cs[i], nat[i]);
        detail::setMacAddress(*cs[i], macs[i]);
    }
    registerAll({&a, &b, &c}, {names[0], names[1], names[2]}, password);
    HostOptions opt;
    opt.name = "NAT test";
    opt.autoStart = false;
    a.host(opt);
    waitFor(a, "hosting", [](const Snapshot& s) { return s.inLobby; });
    const uint32_t lobby = a.snapshot().lobby.id;
    b.join(lobby);
    c.join(lobby);
    for (Client* x : cs) waitFor(*x, "player, link up", [](const Snapshot& s) { return s.player && s.linkUp; });
    for (Client* x : cs) waitFor(*x, "public address (STUN through the NAT)", [](const Snapshot& s) { return s.publicAddress; });
    waitFor(a, "A-B direct (hole punched)", [&](const Snapshot& s) {
        const Member* m = memberNamed(s, names[1]);
        return m && m->path == Path::DirectInternet;
    });
    waitFor(b, "B-A direct", [&](const Snapshot& s) {
        const Member* m = memberNamed(s, names[0]);
        return m && m->path == Path::DirectInternet;
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));  // C's punching gets nowhere: still the relay
    CHECK(pathTo(a, names[2]) == Path::Relay);
    CHECK(pathTo(b, names[2]) == Path::Relay);
    CHECK(pathTo(c, names[0]) == Path::Relay);
    CHECK(pathTo(c, names[1]) == Path::Relay);
    CHECK(a.snapshot().relayTcp);  // (the master's UDP gets nowhere)
    for (Client* x : cs) {
        Snapshot s = x->snapshot();
        std::printf("%s:", s.me.username.c_str());
        for (auto& m : s.lobby.members)
            if (m.name != s.me.username) std::printf(" %s %s %d ms;", m.name.c_str(), pathName(m.path), m.peerRttMs);
        std::printf("\n");
    }
    settle(a, b, macs[0], macs[1]);
    settle(a, c, macs[0], macs[2]);
    settle(b, c, macs[1], macs[2]);
    // A broadcast reaches each other console exactly once (B directly, C through the relay) ...
    auto f1 = ethernet(bcast, macs[0], "A to everyone");
    a.port().send(f1.data(), f1.size());
    CHECK(countArrivals(b.port(), f1, 400) == 1);
    CHECK(countArrivals(c.port(), f1, 400) == 1);
    // ... and so does C's (both through the relay); unicasts go to their MAC's console only.
    auto f2 = ethernet(bcast, macs[2], "C to everyone");
    c.port().send(f2.data(), f2.size());
    CHECK(countArrivals(a.port(), f2, 400) == 1);
    CHECK(countArrivals(b.port(), f2, 400) == 1);
    auto f3 = ethernet(macs[0], macs[1], "B to A, directly");
    b.port().send(f3.data(), f3.size());
    CHECK(countArrivals(a.port(), f3, 400) == 1);
    CHECK(countArrivals(c.port(), f3, 200) == 0);
    auto f4 = ethernet(macs[0], macs[2], "C to A, relayed");
    c.port().send(f4.data(), f4.size());
    CHECK(countArrivals(a.port(), f4, 400) == 1);
    CHECK(countArrivals(b.port(), f4, 200) == 0);
    roundTrips(a, b, macs[0], macs[1], "A-B peer to peer through two NATs");
    roundTrips(a, c, macs[0], macs[2], "A-C through the relay (TCP)");
    // A match with the three: the same start for everyone, in console order.
    a.startMatch(true);
    for (Client* x : cs) waitFor(*x, "match start", [](const Snapshot& s) { return s.matchSerial == 1; });
    for (int i = 0; i < 3; i++) {
        const MatchStart& m = cs[i]->snapshot().match;
        CHECK(m.consoles.size() == 3 && m.myConsole == i && m.seed == a.snapshot().match.seed);
        CHECK(std::memcmp(m.consoles[size_t(i)].mac, macs[i], 6) == 0);
    }
    for (Client* x : cs) x->disconnect();
    if (g_failures) {
        std::printf("AIRNET TEST FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("AIRNET TEST PASSED\n");
    return 0;
}

// ---- City Trial maps: the host's map downloaded by the consoles without it ----
static void writeFile(const std::filesystem::path& p, size_t size, uint32_t seed) {
    std::filesystem::create_directories(p.parent_path());
    std::vector<char> b(size);
    for (size_t i = 0; i < size; i++) seed = seed * 1664525u + 1013904223u, b[i] = char(seed >> 24);
    std::ofstream(p, std::ios::binary).write(b.data(), std::streamsize(b.size()));
}

// A hosts with a map; B (a player: peer to peer on this PC) has none, C (waiting: through the relay, over TCP or UDP) an
// older map of the same name. Both get A's, checked and put in place; the master hears each has it, and the match
// starts. Then a map A does not have: B's download fails, and says so.
static int mapTest(const std::string& prefix, bool tcp) {
    namespace fs = std::filesystem;
    const std::string password = "correct horse";
    const std::string names[3] = {prefix + "_host", prefix + "_bee", prefix + "_sea"};
    const fs::path root = fs::absolute("maptest");
    fs::remove_all(root);
    const fs::path dirs[3] = {root / "a", root / "b", root / "c"};
    const fs::path map = dirs[0] / "Test Map!";  // (a name a download keeps as its letters and digits: TestMap)
    writeFile(map / "GrCity1.dat", 700 * 1000 + 17, 1);
    writeFile(map / "GrCity1Model.dat", 1100 * 1000, 2);
    writeFile(map / "audio" / "jp" / "city.hps", 300 * 1000 + 5, 3);
    writeFile(map / "minimap_0.bin", 1200, 4);  // (exactly a chunk's worth)
    std::ofstream(map / "mod.json") << "{\"name\": \"Test Map\"}";
    writeFile(dirs[2] / "TestMap" / "GrCity1.dat", 5000, 9);  // C's older map of that name
    fs::create_directories(dirs[1]);
    std::string hash;
    uint64_t size = 0;
    CHECK(rt::mapfiles::folderHash(rt::mapfiles::dirOf(dirs[0].string(), "Test Map!"), hash, size));
    CHECK(hash.size() == 64 && size == 700 * 1000 + 17 + 1100 * 1000 + 300 * 1000 + 5 + 1200 + 20);
    {  // (packed and unpacked here first: what goes over the network)
        std::vector<uint8_t> packed;
        uint32_t manifest = 0;
        std::string packedHash, err, folder;
        CHECK(rt::mapfiles::pack(rt::mapfiles::dirOf(dirs[0].string(), "Test Map!"), packed, manifest, packedHash) && packedHash == hash);
        packed[manifest + 5] ^= 1;  // a byte changed on the way: refused
        CHECK(!rt::mapfiles::install((root / "x").string(), "Test Map!", hash, packed, manifest, err, &folder) && !err.empty());
        CHECK(!fs::exists(root / "x" / "TestMap") && !fs::exists(root / "x" / ".dl-TestMap"));
    }
    Client a, b, c;
    Client* cs[3] = {&a, &b, &c};
    for (int i = 0; i < 3; i++) detail::setMapsFolder(*cs[i], dirs[i].string());
    registerAll({&a, &b, &c}, {names[0], names[1], names[2]}, password);
    c.forceRelayTcp(tcp);
    HostOptions opt;
    opt.name = "Map test";
    opt.maxPlayers = 2;  // (C waits: not on the roster, it gets the map through the relay all the same)
    opt.autoStart = false;
    opt.mode = Mode::CityTrial;
    char settings[512];
    std::snprintf(settings, sizeof settings, R"({"game":"citytrial","map":{"folder":"Test Map!","title":"Test Map","hash":"%s","size":%llu}})",
                  hash.c_str(), static_cast<unsigned long long>(size));
    opt.settingsJson = settings;
    a.host(opt);
    waitFor(a, "hosting", [](const Snapshot& s) { return s.inLobby; });
    const uint32_t lobby = a.snapshot().lobby.id;
    b.join(lobby);
    waitFor(b, "B a player", [](const Snapshot& s) { return s.player; });
    c.join(lobby);
    waitFor(a, "A has its own map", [](const Snapshot& s) { return s.mapState == MapState::Ready && s.mapTitle == "Test Map"; });
    waitFor(b, "B direct to A", [&](const Snapshot& s) {
        const Member* m = memberNamed(s, names[0]);
        return m && m->path == Path::DirectLan;
    });
    const auto t0 = Clock::now();
    waitFor(b, "B has the map", [&](const Snapshot& s) { return s.mapState == MapState::Ready; }, 60000);
    const auto tb = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    float seen = 0;
    waitFor(c, "C has the map", [&](const Snapshot& s) {  // (through the relay, paced: its progress seen on the way)
        if (s.mapState == MapState::Downloading && s.mapProgress > 0 && s.mapProgress < 1) seen = s.mapProgress;
        return s.mapState == MapState::Ready;
    }, 90000);
    const auto tc = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("map download: %llu bytes, B peer to peer %lld ms, C through the relay (%s) %lld ms (C seen at %.0f%%)\n",
                static_cast<unsigned long long>(size), static_cast<long long>(tb), tcp ? "TCP" : "UDP", static_cast<long long>(tc), seen * 100.f);
    CHECK(seen > 0);
    CHECK(c.snapshot().relayTcp == tcp);
    for (int i = 1; i < 3; i++) {
        std::string h;
        uint64_t n = 0;
        CHECK(rt::mapfiles::folderHash(rt::mapfiles::dirOf(dirs[i].string(), "TestMap"), h, n) && h == hash);  // (C's older one replaced)
        CHECK(rt::mapfiles::find(dirs[i].string(), "Test Map!", hash) == "TestMap");
        for (const auto& e : fs::directory_iterator(dirs[i])) CHECK(e.path().filename() == "TestMap");  // (nothing left over)
    }
    // the master knows: every member's map is the settings' - and the match starts
    waitFor(a, "every member has the map", [&](const Snapshot& s) {
        return s.lobby.members.size() == 3 &&
               std::all_of(s.lobby.members.begin(), s.lobby.members.end(), [&](const Member& m) { return m.mapOk == hash; });
    });
    a.startMatch(true);
    waitFor(b, "match start", [](const Snapshot& s) { return s.matchSerial == 1; });
    CHECK(b.snapshot().match.settingsJson.find(hash) != std::string::npos);
    a.endMatch("{}");
    waitFor(a, "results", [](const Snapshot& s) { return s.lobby.session == SessionState::Results; });
    a.continueSession();
    waitFor(a, "lobby again", [](const Snapshot& s) { return s.lobby.session == SessionState::Lobby; });
    // a map the host does not have: B asks, A says so, B shows why (and the match waits)
    std::snprintf(settings, sizeof settings, R"({"game":"citytrial","map":{"folder":"Nowhere","title":"Nowhere","hash":"%s","size":10}})",
                  std::string(64, 'e').c_str());
    a.setGameSettings(settings);
    waitFor(b, "B cannot get it", [](const Snapshot& s) { return s.mapState == MapState::Failed && !s.mapError.empty() && s.mapTitle == "Nowhere"; },
            20000);
    std::printf("a map nobody has: \"%s\"\n", b.snapshot().mapError.c_str());
    a.startMatch(true);
    waitFor(a, "no match without it", [](const Snapshot& s) { return s.error.find("map") != std::string::npos; });
    // the city again: nothing to get
    a.setGameSettings(R"({"game":"citytrial"})");
    waitFor(b, "no map", [](const Snapshot& s) { return s.mapState == MapState::None; });
    for (Client* x : cs) x->disconnect();
    fs::remove_all(root);
    if (g_failures) {
        std::printf("AIRNET TEST FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("AIRNET TEST PASSED\n");
    return 0;
}

int main(int argc, char** argv) {
    std::string master = "127.0.0.1:27600";
    std::string prefix = "cpp";
    std::string stun;  // (none: never a real STUN server from tests)
    std::string nat[3];
    bool udpBlocked = false;
    int maps = 0;  // (1: the map test, C on the TCP relay; 2: on UDP)
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--master")) master = argv[i + 1];
        if (!std::strcmp(argv[i], "--prefix")) prefix = argv[i + 1];
        if (!std::strcmp(argv[i], "--udp-blocked")) udpBlocked = std::atoi(argv[i + 1]) != 0;
        if (!std::strcmp(argv[i], "--maps")) maps = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--stun")) stun = argv[i + 1];
        for (int k = 0; k < 3; k++) {
            const std::string flag = std::string("--nat-") + char('a' + k);
            if (argv[i] == flag) nat[k] = argv[i + 1];
        }
    }
    const std::string userA = prefix + "_alice", userB = prefix + "_bob";
    const std::string password = "correct horse";

    cryptoVectors();

    rt::settings::onlineMaster.set(master);  // where calls connect to by themselves
    rt::settings::onlineStun.set(stun);
    if (!nat[0].empty()) return natTest(master, prefix, nat);
    if (maps) return mapTest(prefix, maps == 1);

    const uint8_t macA[6] = {0x02, 0x4B, 0x41, 0x52, 0x00, 0x0A};
    const uint8_t macB[6] = {0x02, 0x4B, 0x41, 0x52, 0x00, 0x0B};
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Client a, b;
    detail::setMacAddress(a, macA);
    detail::setMacAddress(b, macB);
    // ---- accounts: A connects explicitly; B's register connects on its own ----
    a.connect(master, false);
    waitFor(a, "A connected", [](const Snapshot& s) { return s.state == State::Connected; });
    a.registerAccount(userA, password, false);
    b.registerAccount(userB, password, false);
    waitFor(a, "A online", [](const Snapshot& s) { return s.state == State::Online && s.loggedIn; });
    waitFor(b, "B online", [](const Snapshot& s) { return s.state == State::Online; });
    Snapshot sa = a.snapshot();
    CHECK(sa.me.username == userA);
    CHECK(sa.serverVersion.rfind("airride-master", 0) == 0);

    // A bad login is reported, not fatal.
    {
        Client c;
        c.connect(master, false);
        c.login(userA, "wrong password", false);
        waitFor(c, "bad login error", [](const Snapshot& s) { return !s.error.empty() && !s.busy; });
        CHECK(c.snapshot().state == State::Connected);
        CHECK(c.snapshot().error.find("Wrong username or password") != std::string::npos);
    }

    // ---- profile ----
    a.setProfile("yellow", "dragoon");
    waitFor(a, "profile set", [](const Snapshot& s) { return s.me.colour == "yellow" && s.me.machine == "dragoon"; });
    b.viewProfile(userA);
    waitFor(b, "profile viewed", [](const Snapshot& s) { return s.viewedValid && s.viewed.colour == "yellow"; });

    // ---- host, browse, join: in the server = in the match ----
    HostOptions opt;
    opt.name = "C++ test server";
    opt.maxMembers = 4;
    opt.mode = Mode::CityTrial;
    opt.region = "Local";
    opt.autoStart = false;  // (this test starts the match itself)
    opt.settingsJson = R"({"mode":"city_trial","minutes":5})";
    a.host(opt);
    waitFor(a, "hosting", [](const Snapshot& s) { return s.inLobby && s.isHost && s.player; });
    const uint32_t lobbyId = a.snapshot().lobby.id;
    CHECK(a.snapshot().lobby.settingsJson == R"({"mode":"city_trial","minutes":5})");
    b.refreshServers();
    waitFor(b, "server listed", [&](const Snapshot& s) {
        return std::any_of(s.servers.begin(), s.servers.end(), [&](const ServerInfo& x) {
            return x.id == lobbyId && x.name == "C++ test server" && x.mode == Mode::CityTrial && x.max == 4 &&
                   x.session == SessionState::Lobby && x.maxPlayers == kMatchMax;
        });
    });
    b.join(lobbyId);
    waitFor(b, "joined: a player at once", [](const Snapshot& s) { return s.inLobby && s.player && s.lobby.members.size() == 2; });
    waitFor(a, "A sees B", [](const Snapshot& s) { return s.lobby.members.size() == 2; });
    waitFor(a, "A link up", [](const Snapshot& s) { return s.linkUp; });
    waitFor(b, "B link up", [](const Snapshot& s) { return s.linkUp; });

    // ---- chat ----
    b.chat("hello from \"b\"\n\x01");
    waitFor(a, "chat", [](const Snapshot& s) {
        return !s.chat.empty() && s.chat.back().text == "hello from \"b\"";
    });

    std::vector<uint8_t> got;
    auto f1 = ethernet(bcast, macA, "broadcast from A");
    auto f2 = ethernet(macA, macB, "unicast B->A");
    auto big = ethernet(bcast, macA, std::string(1500, 'x'));  // 1514 bytes: the largest frame

    // ---- peer to peer: both on this PC, so the LAN address works ----
    waitFor(a, "A->B direct", [&](const Snapshot& s) {
        const Member* m = memberNamed(s, userB);
        return m && m->path == Path::DirectLan && m->peerRttMs >= 0;
    });
    waitFor(b, "B->A direct", [&](const Snapshot& s) {
        const Member* m = memberNamed(s, userA);
        return m && m->path == Path::DirectLan;
    });
    a.port().send(f1.data(), f1.size());
    CHECK(countArrivals(b.port(), f1, 300) == 1);  // once: not also through the relay
    b.port().send(f2.data(), f2.size());
    CHECK(receive(a.port(), got) && got == f2);
    a.port().send(big.data(), big.size());
    CHECK(receive(b.port(), got) && got == big);
    roundTrips(a, b, macA, macB, "peer to peer (LAN)");

    // ---- the relay: peer to peer off ----
    a.enableP2p(false);
    b.enableP2p(false);
    waitFor(a, "A->B through the relay", [&](const Snapshot& s) { return !s.p2p && pathTo(a, userB) == Path::Relay; });
    waitFor(b, "B->A through the relay", [&](const Snapshot& s) { return !s.p2p && pathTo(b, userA) == Path::Relay; });
    waitFor(a, "A link (relay)", [](const Snapshot& s) { return s.linkUp; });
    waitFor(b, "B link (relay)", [](const Snapshot& s) { return s.linkUp; });
    settle(a, b, macA, macB);
    a.port().send(f1.data(), f1.size());
    CHECK(countArrivals(b.port(), f1, 300) == 1);
    b.port().send(f2.data(), f2.size());
    CHECK(receive(a.port(), got) && got == f2);
    a.port().send(big.data(), big.size());
    CHECK(receive(b.port(), got) && got == big);

    // The transport: UDP normally; TCP, chosen by itself, when UDP gets no answer.
    CHECK(a.snapshot().relayTcp == udpBlocked);
    CHECK(b.snapshot().relayTcp == udpBlocked);
    roundTrips(a, b, macA, macB, udpBlocked ? "relay, both TCP (UDP blocked: fallback)" : "relay, both UDP");

    if (!udpBlocked) {
        // ---- B on the TCP relay, A on UDP: two transports ----
        b.forceRelayTcp(true);
        waitFor(b, "B over TCP", [](const Snapshot& s) { return s.relayTcp && s.linkUp; });
        settle(a, b, macA, macB);
        auto t1 = ethernet(bcast, macA, "udp -> tcp");
        a.port().send(t1.data(), t1.size());
        CHECK(receive(b.port(), got) && got == t1);
        auto t2 = ethernet(macA, macB, "tcp -> udp");
        b.port().send(t2.data(), t2.size());
        CHECK(receive(a.port(), got) && got == t2);
        b.port().send(big.data(), big.size());
        CHECK(receive(a.port(), got) && got == big);
        roundTrips(a, b, macA, macB, "relay, A UDP, B TCP");

        // ---- both on TCP ----
        a.forceRelayTcp(true);
        waitFor(a, "A over TCP", [](const Snapshot& s) { return s.relayTcp && s.linkUp; });
        roundTrips(a, b, macA, macB, "relay, both TCP");

        // ---- and back to UDP ----
        a.forceRelayTcp(false);
        b.forceRelayTcp(false);
        waitFor(a, "A back on UDP", [](const Snapshot& s) { return !s.relayTcp && s.linkUp; });
        waitFor(b, "B back on UDP", [](const Snapshot& s) { return !s.relayTcp && s.linkUp; });
        settle(a, b, macA, macB);
        auto t3 = ethernet(bcast, macB, "udp again");
        b.port().send(t3.data(), t3.size());
        CHECK(receive(a.port(), got) && got == t3);
    }
    // ---- one console peer to peer, the other not: A's frames to B go through the relay, B's direct
    // attempts get no answer - still exactly once each way ----
    a.enableP2p(true);
    waitFor(a, "A on, B off: relay", [&](const Snapshot& s) { return s.p2p && pathTo(a, userB) == Path::Relay; });
    settle(a, b, macA, macB);
    a.port().send(f1.data(), f1.size());
    CHECK(countArrivals(b.port(), f1, 300) == 1);
    b.port().send(f2.data(), f2.size());
    CHECK(countArrivals(a.port(), f2, 300) == 1);
    b.enableP2p(true);
    waitFor(a, "direct again", [&](const Snapshot&) { return pathTo(a, userB) == Path::DirectLan; });

    Snapshot sb = b.snapshot();
    std::printf("B: relay ping %d ms, frames sent %llu, received %llu\n", sb.pingMs,
                static_cast<unsigned long long>(sb.framesSent), static_cast<unsigned long long>(sb.framesReceived));

    // ---- a dropped connection: B reconnects, resumes, and is back in the lobby and match ----
    detail::simulateConnectionLoss(b);
    waitFor(b, "B reconnecting", [](const Snapshot& s) { return s.state != State::Online; }, 2000);
    waitFor(b, "B back", [&](const Snapshot& s) {
        return s.state == State::Online && s.inLobby && s.player && s.linkUp && s.relayTcp == udpBlocked;
    });
    CHECK(b.snapshot().lobby.id == lobbyId);
    settle(a, b, macA, macB);
    auto f3 = ethernet(bcast, macA, "after the reconnect");
    a.port().send(f3.data(), f3.size());
    CHECK(receive(b.port(), got) && got == f3);

    // ---- the session: settings, the match start, its end ----
    b.setGameSettings("{}");  // not the host
    waitFor(b, "not host error", [](const Snapshot& s) { return s.error.find("host") != std::string::npos; });
    a.setGameSettings("not json");
    waitFor(a, "bad settings error", [](const Snapshot& s) { return s.error.find("not valid") != std::string::npos; });
    a.setGameSettings(R"({"mode": "city_trial", "minutes": 7, "items": [1, 2, 3]})");
    waitFor(b, "settings seen", [](const Snapshot& s) { return s.lobby.settingsJson == R"({"mode":"city_trial","minutes":7,"items":[1,2,3]})"; });
    a.startMatch(true);
    waitFor(a, "A match start", [](const Snapshot& s) { return s.matchSerial == 1; });
    waitFor(b, "B match start", [](const Snapshot& s) { return s.matchSerial == 1; });
    {
        const MatchStart ma = a.snapshot().match, mb = b.snapshot().match;
        CHECK(ma.id == mb.id && ma.seed == mb.seed && ma.startAtUnixMs == mb.startAtUnixMs);
        CHECK(ma.consoles.size() == 2 && ma.consoles[0].name == userA && ma.consoles[1].name == userB);
        CHECK(ma.myConsole == 0 && mb.myConsole == 1 && ma.consoles[0].me && !ma.consoles[1].me);
        CHECK(std::memcmp(ma.consoles[1].mac, macB, 6) == 0 && ma.consoles[1].peerId != 0);
        CHECK(ma.settingsJson == R"({"mode":"city_trial","minutes":7,"items":[1,2,3]})" && ma.mode == Mode::CityTrial);
        const int64_t lead = ma.startAtMs - clockMs();
        CHECK(lead > -5000 && lead < 5000);
        std::printf("match %u: seed %u, consoles %s, %s; starts in %lld ms\n", ma.id, ma.seed, ma.consoles[0].name.c_str(),
                    ma.consoles[1].name.c_str(), static_cast<long long>(lead));
    }
    waitFor(b, "in match", [](const Snapshot& s) { return s.lobby.session == SessionState::InMatch; });
    a.setGameSettings("{}");  // not during a match
    waitFor(a, "in-match error", [](const Snapshot& s) { return s.error.find("between matches") != std::string::npos; });
    b.endMatch(R"({"place": 2})");
    a.endMatch(R"({"place": 1})");
    waitFor(b, "results", [](const Snapshot& s) {
        return s.lobby.session == SessionState::Results && s.lobby.resultsJson.find("\"place\":1") != std::string::npos;
    });
    a.reportMatch(true);
    waitFor(a, "match recorded", [](const Snapshot& s) { return s.me.played == 1 && s.me.wins == 1; });
    a.continueSession();
    waitFor(b, "lobby again", [](const Snapshot& s) { return s.lobby.session == SessionState::Lobby; });
    a.startMatch();
    waitFor(b, "countdown", [](const Snapshot& s) {
        return s.lobby.session == SessionState::Countdown && s.lobby.countdownEndsMs > clockMs();
    });
    a.cancelMatch();
    waitFor(b, "cancelled", [](const Snapshot& s) { return s.lobby.session == SessionState::Lobby && s.lobby.hold; });

    // ---- kick: off the LAN ----
    a.kick(userB);
    waitFor(b, "B kicked", [](const Snapshot& s) {
        return !s.inLobby && !s.linkUp && s.error.find("removed") != std::string::npos;
    });
    a.port().send(f1.data(), f1.size());
    CHECK(!receive(b.port(), got, 300));

    // ---- remembered login: saved, then resumed by connect() ----
    a.leave();
    waitFor(a, "A left", [](const Snapshot& s) { return !s.inLobby; });
    {
        Client c;
        c.login(userA, password, true);  // connects to settings::onlineMaster
        waitFor(c, "C logged in (remember)", [](const Snapshot& s) { return s.loggedIn && s.savedLogin; });
        CHECK(rt::settings::onlineUser.get() == userA);
        CHECK(rt::settings::onlineToken.get().size() == 48);
        c.disconnect();
        waitFor(c, "C offline", [](const Snapshot& s) { return s.state == State::Offline; });
        c.connect(master, true);
        waitFor(c, "C resumed", [&](const Snapshot& s) { return s.state == State::Online && s.me.username == userA; });
        c.logout();
        waitFor(c, "C logged out", [](const Snapshot& s) { return !s.loggedIn && s.state == State::Connected; });
        CHECK(rt::settings::onlineToken.get().empty());
    }

    a.disconnect();
    b.disconnect();
    waitFor(a, "A offline", [](const Snapshot& s) { return s.state == State::Offline; });

    // Nothing the player can see names the master (address or port).
    for (Client* c : {&a, &b}) {
        const std::string err = c->snapshot().error;
        const std::string host = master.substr(0, master.rfind(':')), port = master.substr(master.rfind(':') + 1);
        CHECK(err.find(host) == std::string::npos && err.find(port) == std::string::npos);
    }
    {
        // "localhost" resolves to ::1 first on most systems; the test server listens on 127.0.0.1 only:
        // the client must go on to the next address.
        Client c;
        c.connect("localhost:" + master.substr(master.rfind(':') + 1), false);
        waitFor(c, "connected by name (every resolved address tried)", [](const Snapshot& s) { return s.state == State::Connected; });
    }
    {
        Client c;
        c.connect("127.0.0.1:1", false);  // nothing listens there
        waitFor(c, "unreachable master reported", [](const Snapshot& s) { return s.state == State::Offline && !s.error.empty(); });
        const std::string err = c.snapshot().error;
        std::printf("unreachable master: \"%s\"\n", err.c_str());
        CHECK(err.find("127.0.0.1") == std::string::npos && err.find(":1") == std::string::npos);
    }

    if (g_failures) {
        std::printf("AIRNET TEST FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("AIRNET TEST PASSED\n");
    return 0;
}
