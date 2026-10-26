#pragma once
// First-run setup: builds the game's code from the player's own disc. Nothing translated from the game ships
// with the program - it makes kirbyairride_game.dll (the 8-rider build's: kirbyairride_game_8p.dll) on this
// computer:
//
//   1. Verify    main.dol, read from the disc image, against the known program's SHA-1
//   2. Recompile the PowerPC code to C++ (src/recompiler, linked into the program); the 8-rider build with
//                config/GKYE01/patches_8p.txt
//   3. Compile   that C++ with the bundled clang (tools/llvm), one process per processor thread
//   4. Link      the module with the bundled lld-link, against kirbyairride.lib (the runtime's exports)
//
// It runs on its own thread; the setup screen (src/runtime/setupscreen.cpp) draws status() each frame, and
// headless runs print it (docs/SETUP.md has the details).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {

class Setup {
public:
    enum Step { Verify, Recompile, Compile, Link, kSteps };
    static const char* stepName(int s);

    struct Status {
        bool running = false, done = false, failed = false, cancelled = false;
        int step = Verify;
        float stepFrac = 0;   // 0-1 within the step
        float overall = 0;    // 0-1 over all steps
        std::string detail;   // what the step is on, e.g. "12 of 27 files"
        std::string error;    // why it failed
        float elapsed = 0;    // seconds
        float eta = -1;       // seconds left (-1: not known yet)
    };

    ~Setup();
    // The module's file and its stamp (what it was built by and from) beside the program.
    static std::filesystem::path modulePath(const std::filesystem::path& exeDir, bool eightRiders);
    // Whether the module has to be built (again): missing, or built by another version of the program or from
    // another disc image. (A module with no stamp is a developer build, made by CMake: taken as it is.)
    static bool needed(const std::filesystem::path& exeDir, const std::filesystem::path& disc, bool eightRiders);
    // Where the bundled compiler is (tools/llvm beside the program).
    static std::filesystem::path toolchain(const std::filesystem::path& exeDir);

    void start(const std::filesystem::path& exeDir, const std::filesystem::path& disc, bool eightRiders);
    void cancel();
    Status status() const;
    bool running() const { return m_running.load(); }
    std::filesystem::path logFile() const { return m_logFile; }

private:
    void run();
    bool verify(std::vector<uint8_t>& dol, std::string& sha1);
    bool recompile(const std::vector<uint8_t>& dol);
    bool compile();
    bool link();
    void set(int step, float frac, const std::string& detail);
    bool fail(const std::string& why);
    void log(const std::string& line);

    std::filesystem::path m_exeDir, m_disc, m_work, m_logFile;
    bool m_eightRiders = false;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_cancel{false};
    mutable std::mutex m_lock;
    Status m_status;
    std::chrono::steady_clock::time_point m_t0;
    std::vector<std::filesystem::path> m_objects;
};

}  // namespace rt
