#pragma once
// The player's settings (src/runtime/settings.cpp): changed in the menu (src/runtime/menu.cpp), read
// live by the frontend, kept in kirbyairride-settings.ini beside the program.
#include <atomic>
#include <mutex>
#include <string>

namespace rt::settings {

// A text setting (no spaces; up to 63 characters), read and written whole from any thread.
class Text {
public:
    explicit Text(const char* value) : value_(value) {}
    std::string get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }
    void set(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = value;
    }

private:
    mutable std::mutex mutex_;
    std::string value_;
};

// Graphics
extern std::atomic<bool> interpolation;  // in-between frames on displays faster than 60 Hz
extern std::atomic<int> frameLimit;      // frames drawn a second at most (0: the display's rate)
extern std::atomic<bool> showFps;        // the frame counter in a corner
extern std::atomic<bool> fullscreen;
extern std::atomic<bool> widescreen;     // the window's shape (else 4:3, pillarboxed)
// Audio
extern std::atomic<float> masterVolume;  // 0..1
extern std::atomic<bool> muteUnfocused;  // silent while the window is in the background
// Interface
extern std::atomic<bool> menuHint;       // "F1: Menu" shown at start
// City Trial
extern Text cityMap;                     // the map played in the city's place: a map mod's folder (mods/maps/<folder>);
                                         // empty: the city itself
// Online (src/runtime/airnet.cpp)
extern Text onlineMaster;                // the master server, "host" or "host:port"
extern Text onlineServer;                // a dedicated server chosen instead (the same server software, run by
                                         // anyone: server/); empty: the master
extern Text onlineUser;                  // the login remembered on this PC: username ...
extern Text onlineToken;                 // ... and session token (hex; empty: none)
extern Text onlineTokenServer;           // ... for which server (empty: the master)
extern std::atomic<bool> onlineRelayTcp; // the relay over TCP even if UDP works (else: only when UDP fails)
extern std::atomic<bool> onlineP2p;      // consoles connect peer to peer where they can (else all through the relay)
extern Text onlineStun;                  // STUN servers (host:port,host:port; empty: none) for the public address

void load();
void save();  // (after a change; cheap)

}  // namespace rt::settings
