#include "runtime/setup.h"

#include "disc/discimage.h"
#include "recompiler/analyze.h"
#include "recompiler/emit.h"
#include "runtime/abi.h"
#include "runtime/sha1.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef KAR_VERSION
#define KAR_VERSION "dev"
#endif

namespace rt {

namespace fs = std::filesystem;

namespace {

struct EmbeddedFile {
    const char* path;
    const unsigned char* data;
    size_t size;
};
#include "setup_files.inc"  // the module's headers and glue, the names, the 8-rider overrides (tools/embed.cmake)

std::string embedded(const char* path) {
    for (const EmbeddedFile& f : kEmbedded)
        if (!std::strcmp(f.path, path)) return std::string(reinterpret_cast<const char*>(f.data), f.size);
    return {};
}

constexpr const char* kGameId = "GKYE01";
// The program the recompiler is made for: main.dol of Kirby Air Ride (USA), 5,514,016 bytes. Disc images come
// in many forms (.iso, .ciso, .rvz, trimmed) that all hold the same program, so it is the program that is checked.
constexpr const char* kProgramSha1 = "977533bb96e6a16e127946396489be1b4164ac42";
// Each step's share of the overall bar: about its share of the time (8 threads: disc under a second,
// recompiling ~30 s, compiling ~130 s, linking under a second).
constexpr float kWeight[Setup::kSteps] = {0.01f, 0.18f, 0.80f, 0.01f};
constexpr const char* kClang = "clang.exe";
constexpr const char* kLinker = "lld-link.exe";
constexpr const char* kImportLib = "kirbyairride.lib";

const char* dllName(bool eightRiders) { return eightRiders ? "kirbyairride_game_8p.dll" : "kirbyairride_game.dll"; }
fs::path stampPath(const fs::path& exeDir, bool eightRiders) {
    return exeDir / (eightRiders ? "kirbyairride_game_8p.txt" : "kirbyairride_game.txt");
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// kirbyairride_game.txt: what the module next to it was built by and from.
struct Stamp {
    std::string version, disc, sha1;
    uint64_t size = 0;
    int64_t time = 0;
    bool operator==(const Stamp& o) const = default;
};
Stamp readStamp(const fs::path& p) {
    Stamp s;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "version") s.version = v;
        else if (k == "disc") s.disc = v;
        else if (k == "sha1") s.sha1 = v;
        else if (k == "size") s.size = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "time") s.time = std::strtoll(v.c_str(), nullptr, 10);
    }
    return s;
}
// The disc as the stamp knows it: its path, size and time (checked again whenever one changes).
Stamp discIdentity(const fs::path& disc) {
    Stamp s;
    std::error_code ec;
    s.version = std::string(KAR_VERSION) + "/abi" + std::to_string(KAR_GAME_ABI);
    s.disc = u8(disc);
    s.size = fs::file_size(disc, ec);
    s.time = int64_t(fs::last_write_time(disc, ec).time_since_epoch().count());
    return s;
}
void writeStamp(const fs::path& p, const Stamp& s) {
    std::ofstream f(p);
    f << "version=" << s.version << "\ndisc=" << s.disc << "\nsize=" << s.size << "\ntime=" << s.time << "\nsha1=" << s.sha1 << "\n";
}

std::string fmtBytes(uint64_t n) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f MB", double(n) / 1048576.0);
    return b;
}

// A child process (the compiler, the linker) with its output going to a log file. Only that file's handle is
// inherited, so the many children never hold each other's logs open.
struct Child {
    HANDLE proc = nullptr;
    size_t index = 0;
    fs::path log;
};
bool spawn(HANDLE job, const fs::path& exe, const std::wstring& args, const fs::path& dir, const fs::path& logPath, Child& c, std::string& err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return err = "can't write " + u8(logPath), false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    si.lpAttributeList = attrs;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                                   dir.c_str(), &si.StartupInfo, &pi);
    const DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(out);
    if (!ok) return err = "couldn't start " + u8(exe.filename()) + " (error " + std::to_string(e) + ")", false;
    AssignProcessToJobObject(job, pi.hProcess);  // ends with the program, whatever happens to it
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    c.log = logPath;
    return true;
}
HANDLE makeJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    return job;
}

std::string readText(const fs::path& p, size_t maxLines) {
    std::ifstream f(p);
    std::string out;
    size_t n = 0;
    for (std::string l; n < maxLines && std::getline(f, l); n++) out += l + "\n";
    return out;
}

}  // namespace

const char* Setup::stepName(int s) {
    static const char* names[kSteps] = {"Checking your disc", "Recompiling the game to C++", "Compiling the game", "Linking the game"};
    return s >= 0 && s < kSteps ? names[s] : "";
}

Setup::~Setup() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

fs::path Setup::modulePath(const fs::path& exeDir, bool eightRiders) { return exeDir / dllName(eightRiders); }

fs::path Setup::toolchain(const fs::path& exeDir) { return exeDir / "tools" / "llvm"; }

bool Setup::needed(const fs::path& exeDir, const fs::path& disc, bool eightRiders) {
    std::error_code ec;
    if (!fs::exists(modulePath(exeDir, eightRiders), ec)) return true;
    if (!fs::exists(stampPath(exeDir, eightRiders), ec)) return false;  // a developer build
    Stamp want = discIdentity(disc), have = readStamp(stampPath(exeDir, eightRiders));
    want.sha1 = have.sha1;
    return !(want == have);
}

void Setup::start(const fs::path& exeDir, const fs::path& disc, bool eightRiders) {
    if (m_running) return;
    if (m_thread.joinable()) m_thread.join();
    m_exeDir = exeDir, m_disc = disc, m_eightRiders = eightRiders;
    m_logFile = exeDir / "setup.log";
    const char* la = std::getenv("LOCALAPPDATA");
    m_work = (la ? fs::path(std::u8string(reinterpret_cast<const char8_t*>(la))) / "KirbyAirRideRecompiled" : exeDir) / "build";
    {
        std::lock_guard g(m_lock);
        m_status = {};
        m_status.running = true;
    }
    m_cancel = false;
    m_running = true;
    m_t0 = std::chrono::steady_clock::now();
    std::ofstream(m_logFile, std::ios::trunc);
    m_thread = std::thread([this] { run(); });
}

void Setup::cancel() { m_cancel = true; }

Setup::Status Setup::status() const {
    std::lock_guard g(m_lock);
    Status s = m_status;
    s.elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    // time left from the pace so far, once there is enough of it to go on
    if (s.running && s.overall > 0.08f && s.elapsed > 10) s.eta = s.elapsed * (1 - s.overall) / s.overall;
    return s;
}

void Setup::set(int step, float frac, const std::string& detail) {
    std::lock_guard g(m_lock);
    m_status.step = step;
    m_status.stepFrac = std::clamp(frac, 0.f, 1.f);
    float o = 0;
    for (int i = 0; i < step; i++) o += kWeight[i];
    m_status.overall = std::min(1.f, o + kWeight[step] * m_status.stepFrac);
    m_status.detail = detail;
}

bool Setup::fail(const std::string& why) {
    log("FAILED: " + why);
    std::lock_guard g(m_lock);
    if (m_cancel) m_status.cancelled = true;
    else m_status.failed = true, m_status.error = why;
    return false;
}

void Setup::log(const std::string& line) {
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    char b[32];
    std::snprintf(b, sizeof b, "%7.1f s  ", t);
    std::ofstream(m_logFile, std::ios::app) << b << line << "\n";
}

void Setup::run() {
    const fs::path dll = modulePath(m_exeDir, m_eightRiders), stamp = stampPath(m_exeDir, m_eightRiders);
    log("setup " KAR_VERSION ": building " + u8(dll.filename()) + " from " + u8(m_disc));
    std::error_code ec;
    bool ok = false;
    do {
        // the program's folder must take the module (not so in Program Files, say)
        {
            const fs::path probe = m_exeDir / "kirbyairride_write_test.tmp";
            std::ofstream(probe) << "1";
            if (!fs::exists(probe, ec)) {
                fail("The game's folder can't be written to (" + u8(m_exeDir) + "). Move the game to a folder of your own, such as Documents, and try again.");
                break;
            }
            fs::remove(probe, ec);
        }
        const fs::path tools = toolchain(m_exeDir);
        if (!fs::exists(tools / kClang, ec) || !fs::exists(tools / kLinker, ec) || !fs::exists(m_exeDir / kImportLib, ec)) {
            fail("Files the setup needs are missing (tools\\llvm, kirbyairride.lib). Extract the whole download again.");
            break;
        }
        std::vector<uint8_t> dol;
        std::string sha1;
        if (!verify(dol, sha1)) break;
        Stamp st = discIdentity(m_disc);
        st.sha1 = sha1;
        // the same disc under another name or date, and a module already built from it: nothing to do
        if (const Stamp old = readStamp(stamp); fs::exists(dll, ec) && old.version == st.version && old.sha1 == sha1) {
            log("module already built from this disc");
            writeStamp(stamp, st);
            ok = true;
            break;
        }
        fs::remove_all(m_work, ec);
        fs::create_directories(m_work / "gen", ec);
        if (!recompile(dol)) break;
        dol.clear();
        if (!compile()) break;
        if (!link()) break;
        // in place: the new module, then the stamp that says what it is
        fs::remove(stamp, ec);
        fs::copy_file(m_work / dll.filename(), dll, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fail("Couldn't put " + u8(dll.filename()) + " in the game's folder: " + ec.message() + ". Is the game still running?");
            break;
        }
        writeStamp(stamp, st);
        ok = true;
    } while (false);
    if (!ok && m_cancel) log("cancelled");
    // the work files (several hundred MB) are never kept: a failed build is started over
    fs::remove_all(m_work, ec);
    {
        std::lock_guard g(m_lock);
        m_status.running = false;
        m_status.done = ok;
        if (!ok && m_cancel) m_status.cancelled = true, m_status.failed = false;
        if (ok) m_status.overall = 1, m_status.stepFrac = 1;
    }
    log(ok ? "done" : "stopped");
    m_running = false;
}

// 1. main.dol from the disc image: the header's 0x420 is where it starts, its size is where its last section ends;
//    its SHA-1 against the known program's.
bool Setup::verify(std::vector<uint8_t>& dol, std::string& sha1) {
    set(Verify, 0, "");
    std::string err;
    const auto img = disc::open(m_disc, err);
    if (!img) return fail("Can't read the disc image: " + err + ".");
    char id[7] = {};
    uint8_t h[4], dh[0x100];
    if (!img->read(0, id, 6)) return fail("Can't read the disc image.");
    if (std::strcmp(id, kGameId)) return fail("This disc image is " + std::string(id) + ", not Kirby Air Ride (USA, GKYE01).");
    if (!img->read(0x420, h, 4) || !img->read(be32(h), dh, sizeof dh)) return fail("Can't read the game's program from the disc image.");
    const uint32_t at = be32(h);
    uint32_t size = 0x100;
    for (int i = 0; i < 18; i++) size = std::max(size, be32(dh + i * 4) + be32(dh + 0x90 + i * 4));
    dol.resize(size);
    if (size > (64u << 20) || !img->read(at, dol.data(), size)) return fail("Can't read the game's program from the disc image.");
    Sha1 s;
    s.add(dol.data(), dol.size());
    sha1 = s.hex();
    log(std::string(img->format()) + " image; main.dol " + std::to_string(size) + " bytes, SHA-1 " + sha1);
    if (sha1 != kProgramSha1)
        return fail(std::string("This disc image's program is not the Kirby Air Ride (USA) one the recompiler was made for "
                                "(another revision, or a modified game). Expected main.dol SHA-1 ") + kProgramSha1 + ".");
    set(Verify, 1, "");
    return true;
}

// 2. PowerPC -> C++, in this process (src/recompiler), as tools/recomp/sa2recompile.cpp does it.
bool Setup::recompile(const std::vector<uint8_t>& dol) {
    set(Recompile, 0, "Finding every function");
    sa2rc::Program prog;
    std::string err;
    prog.modules.emplace_back();
    if (!sa2rc::loadDol(dol, prog.modules.back(), err)) return fail("The recompiler couldn't read the game's program: " + err);
    sa2rc::analyze(prog);
    if (m_cancel) return fail("cancelled");
    if (m_eightRiders) {  // the 8-rider build: the game's per-player arrays moved to larger storage (docs/MORE_PLAYERS.md)
        const fs::path p = m_work / "patches_8p.txt";
        const std::string text = embedded("patches_8p.txt");
        std::ofstream(p, std::ios::binary).write(text.data(), std::streamsize(text.size()));
        if (!sa2rc::loadOverrides(u8(p), prog.modules[0], prog.overrides, err)) return fail("The 8-rider build's changes don't fit this program: " + err);
        log("8-rider build: " + std::to_string(prog.overrides.words.size()) + " instruction words, " + std::to_string(prog.overrides.moves.size()) +
            " instructions with moved addresses");
    }
    sa2rc::applyNames(prog, embedded("names.txt"),
                      {"os", "gx", "vi", "pad", "si", "dvd", "ai", "ar", "arq", "dsp", "card", "exi", "ax", "db", "OdemuExi2", "amcstubs", "odenotstub", "TRK"});
    set(Recompile, 0.6f, "Translating to C++");
    sa2rc::EmitStats st;
    std::vector<sa2rc::OutputFile> files = sa2rc::emitProgram(prog, st);
    if (m_cancel) return fail("cancelled");
    log("recompiled " + std::to_string(st.functions) + " functions, " + std::to_string(st.instructions) + " instructions, into " +
        std::to_string(files.size()) + " files");
    uint64_t bytes = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (m_cancel) return fail("cancelled");
        std::ofstream o(m_work / "gen" / files[i].name, std::ios::binary);
        o.write(files[i].text.data(), std::streamsize(files[i].text.size()));
        if (!o) return fail("Couldn't write the recompiled code to " + u8(m_work) + " (is the drive full? It needs about 300 MB).");
        bytes += files[i].text.size();
        set(Recompile, 0.9f + 0.1f * float(i + 1) / float(files.size()), std::to_string(i + 1) + " of " + std::to_string(files.size()) + " files written");
    }
    log("wrote " + fmtBytes(bytes) + " of C++");
    // what the module is compiled with besides: the runtime's headers and the glue
    for (const EmbeddedFile& f : kEmbedded) {
        const fs::path p = m_work / fs::path(f.path);
        if (p.extension() == ".txt") continue;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data), std::streamsize(f.size));
    }
    return true;
}

// 3. Every file with the bundled clang, as many at once as there are processor threads (and memory for them),
//    largest first so the last ones finish together.
bool Setup::compile() {
    set(Compile, 0, "");
    struct Src {
        fs::path rel;  // from the work folder
        uint64_t size;
    };
    std::vector<Src> srcs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(m_work / "gen", ec))
        if (e.path().extension() == ".cpp") srcs.push_back({fs::path("gen") / e.path().filename(), e.file_size(ec)});
    srcs.push_back({"glue.cpp", fs::file_size(m_work / "glue.cpp", ec)});
    std::sort(srcs.begin(), srcs.end(), [](const Src& a, const Src& b) { return a.size > b.size; });
    uint64_t total = 0, done = 0;
    for (const Src& s : srcs) total += s.size;
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    // A compiler takes up to ~1 GB on the largest files here. Memory other programs hold is not counted as lost:
    // Windows pages it out when the compilers need it (one per 2 GB installed); what is free right now can allow more.
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const uint64_t byFree = ms.ullAvailPhys / (1024ull << 20), byInstalled = ms.ullTotalPhys / (2048ull << 20);
    const unsigned byMemory = unsigned(std::max<uint64_t>({1, byFree, byInstalled}));
    const unsigned jobs = std::min({threads, byMemory, 32u});
    log("compiling " + std::to_string(srcs.size()) + " files (" + fmtBytes(total) + ") with " + std::to_string(jobs) + " at a time");
    fs::create_directories(m_work / "obj", ec);
    const fs::path clang = toolchain(m_exeDir) / kClang;
    HANDLE job = makeJob();
    std::vector<Child> active;
    size_t next = 0, finished = 0;
    m_objects.clear();
    auto stopAll = [&] {
        for (Child& c : active) TerminateProcess(c.proc, 1), WaitForSingleObject(c.proc, 5000), CloseHandle(c.proc);
        active.clear();
        CloseHandle(job);
    };
    while (next < srcs.size() || !active.empty()) {
        if (m_cancel) return stopAll(), fail("cancelled");
        while (active.size() < jobs && next < srcs.size()) {
            const Src& s = srcs[next];
            const fs::path obj = fs::path("obj") / s.rel.filename().replace_extension(".obj");
            // (freestanding: the module has no C library; unwind tables: a guest thread ends by unwinding through
            // the game's frames)
            const std::wstring args = L"--target=x86_64-pc-windows-msvc -std=c++20 -O2 -g0 -ffreestanding -nostdinc -fno-exceptions -fno-rtti "
                                      L"-fno-threadsafe-statics -funwind-tables -DKAR_GAME_DLL -Iinclude -Igen -w -c \"" +
                                      s.rel.wstring() + L"\" -o \"" + obj.wstring() + L"\"";
            Child c;
            std::string err;
            if (!spawn(job, clang, args, m_work, m_work / "obj" / s.rel.filename().replace_extension(".log"), c, err)) return stopAll(), fail(err);
            c.index = next++;
            active.push_back(c);
            m_objects.push_back(obj);
        }
        std::vector<HANDLE> hs;
        for (const Child& c : active) hs.push_back(c.proc);
        WaitForMultipleObjects(DWORD(hs.size()), hs.data(), FALSE, 250);
        for (size_t i = 0; i < active.size();) {
            if (WaitForSingleObject(active[i].proc, 0) != WAIT_OBJECT_0) {
                i++;
                continue;
            }
            DWORD code = 1;
            GetExitCodeProcess(active[i].proc, &code);
            CloseHandle(active[i].proc);
            const Child c = active[i];
            active.erase(active.begin() + ptrdiff_t(i));
            if (code != 0) {
                log("compiler error in " + u8(srcs[c.index].rel) + ":\n" + readText(c.log, 12));
                stopAll();
                return fail("The compiler stopped on " + u8(srcs[c.index].rel.filename()) + ". The details are in setup.log.");
            }
            done += srcs[c.index].size;
            finished++;
        }
        set(Compile, float(double(done) / double(total)), std::to_string(finished) + " of " + std::to_string(srcs.size()) + " files");
    }
    CloseHandle(job);
    log("compiled");
    return true;
}

// 4. The module: no C library, no entry point; the runtime's functions from kirbyairride.lib.
bool Setup::link() {
    set(Link, 0, "");
    {
        std::ofstream rsp(m_work / "objs.rsp");
        for (const fs::path& o : m_objects) rsp << "\"" << o.generic_string() << "\"\n";
    }
    const std::string dll = dllName(m_eightRiders);
    const std::wstring args = L"/nologo /dll /noentry /nodefaultlib /machine:x64 /opt:ref /opt:noicf /out:" + fs::path(dll).wstring() +
                              L" @objs.rsp \"" + (m_exeDir / kImportLib).wstring() + L"\"";
    HANDLE job = makeJob();
    Child c;
    std::string err;
    if (!spawn(job, toolchain(m_exeDir) / kLinker, args, m_work, m_work / "link.log", c, err)) return CloseHandle(job), fail(err);
    // the linker doesn't say how far it is: the bar creeps towards the end over about the time it takes
    const auto t0 = std::chrono::steady_clock::now();
    while (WaitForSingleObject(c.proc, 100) == WAIT_TIMEOUT) {
        if (m_cancel) {
            TerminateProcess(c.proc, 1);
            CloseHandle(c.proc), CloseHandle(job);
            return fail("cancelled");
        }
        const float s = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        set(Link, 1 - std::exp(-s / 1.5f), "");
    }
    DWORD code = 1;
    GetExitCodeProcess(c.proc, &code);
    CloseHandle(c.proc), CloseHandle(job);
    if (code != 0) {
        log("linker error:\n" + readText(m_work / "link.log", 20));
        return fail("The linker stopped. The details are in setup.log.");
    }
    std::error_code ec;
    log("linked " + fmtBytes(fs::file_size(m_work / dll, ec)));
    set(Link, 1, "");
    return true;
}

}  // namespace rt
