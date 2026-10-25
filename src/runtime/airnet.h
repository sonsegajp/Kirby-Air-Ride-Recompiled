#pragma once
// Kirby Air Ride online: the client of the Air Ride master server (server/airride_master; protocol
// and architecture in docs/ONLINE.md).
//
// Accounts and profiles, the server browser (hosted servers of up to 16 people) and chat over one
// TCP connection to the master. A server is one session: joining puts you in it, and its first
// kMatchMax members are the next match's consoles (the rest wait). The session goes round
// Lobby -> Countdown (by itself once 2 are in) -> InMatch -> Results -> Lobby; the master sends the
// players a match-start message (console order, settings, seed, start time).
//
// The game's LAN frames go peer to peer over UDP where a direct path works (LAN address > public
// address from STUN, found by hole punching), and through the master's relay (UDP or TCP) to peers
// without one - per peer, mixed in one match. While this player is on the match's roster, the game's
// client installs all this as the LAN port (rt::lan::setPort), so the game's own LAN mode plays over
// the internet.
//
// For the UI: every call returns at once (the work happens on the client's own thread); results,
// errors and server events land in a Snapshot, read once a frame:
//
//     static rt::airnet::Snapshot s;
//     auto& net = rt::airnet::client();
//     net.snapshot(s);                       // cheap when nothing changed
//     if (s.state == rt::airnet::State::Offline && ImGui::Button("Connect")) net.connect();
//     if (!s.error.empty()) ImGui::TextColored(red, "%s", s.error.c_str());
//     for (auto& srv : s.servers) if (ImGui::Selectable(srv.name.c_str())) net.join(srv.id);
//
// Calls that need a connection make one first (login() while offline connects, then logs in).
#include "runtime/lanport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt::airnet {

inline constexpr int kProtoVersion = 2;    // the control protocol this client speaks
inline constexpr int kDefaultPort = 27600; // master's TCP port when the address names none
inline constexpr int kLobbyMax = 16;       // people in a hosted server
inline constexpr int kMatchMax = 4;        // consoles in a match (the game's LAN limit, for now)
inline constexpr int kGroupMax = kMatchMax;  // (old name)
inline constexpr int kUsernameMin = 3, kUsernameMax = 16;
inline constexpr int kPasswordMin = 6;
inline constexpr int kChatMax = 200;
inline constexpr int kSettingsMax = 4096;  // the game settings, as JSON, bytes

// The client's clock (milliseconds, steady): what Lobby::countdownEndsMs, MatchStart::startAtMs ...
// are on.
int64_t clockMs();

enum class State : uint8_t {
    Offline,       // not connected (error says why, if something went wrong)
    Connecting,    // reaching the master
    Connected,     // connected, not logged in
    LoggingIn,     // register / login / resume in progress
    Online,        // logged in
    Reconnecting,  // the connection dropped while logged in: trying again (lobby place is kept)
};
const char* stateName(State s);

// The server's session.
enum class SessionState : uint8_t {
    Lobby,      // gathering (a countdown starts by itself once 2 are in, unless the host held it)
    Countdown,  // the match starts at Lobby::countdownEndsMs
    InMatch,    // locked: whoever joins now waits for the next match
    Results,    // until Lobby::resultsEndsMs (or the host continues), then Lobby again
};
const char* sessionStateName(SessionState s);

enum class Mode : uint8_t { Any, AirRide, CityTrial };
const char* modeName(Mode m);  // "Any", "Air Ride", "City Trial"

// How this console reaches another player's.
enum class Path : uint8_t {
    None,            // (yourself, or not a player)
    Relay,           // through the master (no direct path found / not yet)
    DirectLan,       // peer to peer, on the local network
    DirectInternet,  // peer to peer, across the internet (hole punched)
};
const char* pathName(Path p);  // "-", "Relay", "Direct (LAN)", "Direct"

// Profile choices: ids as the server stores them, with names to show.
struct Choice {
    const char* id;
    const char* name;
};
extern const Choice kColours[];   // Kirby's colours
extern const int kColourCount;
extern const Choice kMachines[];  // Air Ride machines
extern const int kMachineCount;
const char* colourName(const std::string& id);
const char* machineName(const std::string& id);

struct Profile {
    std::string username;
    std::string colour;   // a kColours id
    std::string machine;  // a kMachines id
    int played = 0;
    int wins = 0;
    int64_t created = 0;   // unix time
    int64_t lastSeen = 0;
    bool online = false;   // (viewed profiles)
    uint32_t lobby = 0;    // (viewed profiles) the server they are in, 0: none
};

struct ServerInfo {  // one row of the server browser
    uint32_t id = 0;
    std::string name;
    std::string host;
    std::string region;
    Mode mode = Mode::Any;
    SessionState session = SessionState::Lobby;
    int members = 0;
    int max = 0;
    int maxPlayers = kMatchMax;
    bool locked = false;  // password protected
    int playing = 0;      // consoles in the match on now (0: none)
    int pingMs = -1;      // the host's round trip to the master (-1: unknown)
};

struct Member {
    std::string name;
    std::string colour;
    std::string machine;
    bool host = false;
    bool player = false;   // a console of the next / current match (else waiting)
    uint32_t peerId = 0;
    Path path = Path::None;  // how this console reaches theirs (players only)
    int peerRttMs = -1;      // round trip to their console (Relay: estimated, through the master)
    bool relay = false;      // the master's relay hears from them
    bool relayTcp = false;   // ... over TCP (their network lets no UDP through)
    bool away = false;       // connection dropped; their place is kept for a while
    int pingMs = -1;         // their round trip to the master
    int group = 0;           // (old: 1 if player, else 0)
};

struct Group {  // (old: match groups. The roster appears as group 1.)
    int id = 0;
    std::vector<std::string> members;
    bool playing = false;
};

struct ChatLine {
    std::string from;  // empty for system lines
    std::string text;
    int64_t time = 0;  // unix time
    bool system = false;
};

struct Lobby {
    uint32_t id = 0;
    std::string name;
    std::string host;
    std::string region;
    Mode mode = Mode::Any;
    int max = kLobbyMax;
    int maxPlayers = kMatchMax;  // consoles a match
    bool locked = false;
    bool migrate = true;         // a new host when the host leaves (else the server closes)
    bool autoStart = true;       // the countdown starts by itself once 2 are in
    // the session
    SessionState session = SessionState::Lobby;
    bool hold = false;              // the host cancelled: no countdown by itself until they start one
    int64_t countdownEndsMs = 0;    // (Countdown) clockMs() when the match starts
    int64_t resultsEndsMs = 0;      // (Results) clockMs() when the lobby comes back
    uint32_t matchId = 0;           // the match on now / last (0: none yet)
    std::vector<std::string> matchPlayers;  // its consoles, in console order
    std::string settingsJson = "{}";  // the host's game settings (JSON object; the game defines them)
    std::string resultsJson = "{}";   // (Results) {player name: what their game reported}
    std::vector<Member> members;
    bool freeGroups = true;          // (old)
    std::vector<Group> groups;       // (old: the roster as one group)
};

// The master's word that a match starts: the same for every console in it.
struct MatchStart {
    uint32_t id = 0;
    uint32_t seed = 0;          // shared random seed
    int64_t startAtMs = 0;      // clockMs() when to start (adjusted for the trip from the master)
    int64_t startAtUnixMs = 0;  // ... as the master's wall clock says
    Mode mode = Mode::Any;
    std::string settingsJson = "{}";
    struct Console {
        int console = 0;         // 0..n-1
        std::string name;
        uint32_t peerId = 0;
        uint8_t mac[6] = {};
        bool me = false;
    };
    std::vector<Console> consoles;
    int myConsole = -1;          // this console's number
};

struct Snapshot {
    uint64_t serial = 0;  // changes whenever anything below (but the relay counters) changes

    State state = State::Offline;
    bool busy = false;        // a request is in flight (show a spinner)
    std::string error;        // the last failure, for the player (clearError() empties it)
    uint32_t errorSerial = 0; // bumps with each new error (to show a toast once)
    bool updateRequired = false;  // the master needs a newer game
    std::string serverVersion;
    std::string motd;
    int usersOnline = 0;

    bool loggedIn = false;
    Profile me;
    bool savedLogin = false;  // a login is remembered on this PC (connect() resumes it)

    std::vector<ServerInfo> servers;  // from the last refreshServers()
    uint32_t serversSerial = 0;       // bumps with each refresh

    bool inLobby = false;
    Lobby lobby;
    bool isHost = false;
    bool player = false;              // this console plays the next / current match (else it waits)
    int myGroup = 0;                  // (old: 1 if player)
    std::vector<ChatLine> chat;       // oldest first, at most 100

    MatchStart match;                 // the last match start
    uint32_t matchSerial = 0;         // bumps with each match start (start the game's match then)

    Profile viewed;                   // from viewProfile()
    bool viewedValid = false;

    // Network (read live on every snapshot() call)
    bool linkUp = false;              // on the roster and connected: the game's LAN is "plugged in"
    bool relayTcp = false;            // the relay runs over TCP (UDP got no answer, or forced) - else UDP
    bool p2p = true;                  // peer to peer is on
    bool publicAddress = false;       // STUN found this console's public address
    int pingMs = -1;                  // round trip to the relay
    uint64_t framesSent = 0;
    uint64_t framesReceived = 0;
};

struct HostOptions {
    std::string name;            // empty: "<you>'s server"
    int maxMembers = kLobbyMax;  // 2..16
    int maxPlayers = kMatchMax;  // consoles a match, 2..kMatchMax
    std::string password;        // empty: open
    Mode mode = Mode::Any;
    std::string region;          // up to 12 of A-Z a-z 0-9 space -
    bool autoStart = true;       // the countdown starts by itself once 2 are in
    bool migrateHost = true;
    std::string settingsJson;    // the game settings (a JSON object); empty: none / unchanged
    bool freeGroups = true;      // (old; ignored)
};

class Client;
namespace detail {
void simulateConnectionLoss(Client& client);
void setUdpGateway(Client& client, const std::string& hostPort);
void setMacAddress(Client& client, const uint8_t mac[6]);
}  // namespace detail

// The online service this PC uses: the dedicated server the player chose in Online Settings
// (settings::onlineServer), or else the master (settings::onlineMaster).
std::string serviceAddress();

class Client {
public:
    // installLanPort: install the network as the game's LAN port while on the roster (the game's
    // client does; test clients do not).
    explicit Client(bool installLanPort = false);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ---- connection ----
    // address: "host" or "host:port" ("[v6addr]:port"); empty: serviceAddress(). Every
    // address the name resolves to is tried in turn. The address is never put in anything shown to
    // the player (errors, the Snapshot); only the log names it.
    // autoLogin: resume the login saved on this PC, if any.
    void connect(const std::string& address = {}, bool autoLogin = true);
    void disconnect();  // leaves the lobby first

    // ---- account ----
    // remember: keep the login on this PC (settings: online_user / online_token, and online_token_server:
    // a login is kept for one server, and offered only when connected to that one).
    void registerAccount(const std::string& username, const std::string& password, bool remember = true);
    void login(const std::string& username, const std::string& password, bool remember = true);
    void resumeSaved();  // log in with the saved token
    void logout();       // ends the session and forgets the saved login
    void changePassword(const std::string& newPassword);

    // ---- profiles ----
    void setProfile(const std::string& colour, const std::string& machine);
    void viewProfile(const std::string& username);  // -> Snapshot::viewed
    void reportMatch(bool won);                     // after a match played online

    // ---- server browser ----
    void refreshServers(Mode filter = Mode::Any);
    void host(const HostOptions& options);
    void updateServer(const HostOptions& options);  // host only; empty password: keep it ...
    void clearServerPassword();                     // ... this opens the server
    void join(uint32_t serverId, const std::string& password = {});
    void leave();
    void kick(const std::string& username);  // host only; they cannot come back to this server

    // ---- the session ----
    void startMatch(bool now = false);  // host: start the countdown (now: the match at once)
    void cancelMatch();                 // host: stop the countdown / abandon the match (holds the auto countdown)
    void continueSession();             // host: end the results early
    // This console's game says the match is over, with what it reports (a JSON object). The
    // host's report ends the match; otherwise every player's.
    void endMatch(const std::string& resultsJson = "{}");
    // host: the game settings (a JSON object, up to kSettingsMax bytes; not during a match).
    void setGameSettings(const std::string& json);

    // ---- old match-group calls (kept so the menu builds; they do nothing now) ----
    void joinGroup(int) {}
    void leaveGroup() {}
    void moveToGroup(const std::string&, int) {}
    void arrangeGroups(const std::vector<std::vector<std::string>>&) {}
    void autoFill() {}
    void setPlaying(bool) {}

    // ---- chat ----
    void chat(const std::string& text);

    // ---- network ----
    // The relay normally runs over UDP and falls back to TCP (the master's control port) by itself
    // when UDP gets no answer within 1.5 s. Forcing TCP: settings::onlineRelayTcp, or per client here
    // (overrides the setting; takes effect at once).
    void forceRelayTcp(bool force);
    // Peer to peer (on by default: settings::onlineP2p); off: everything through the relay.
    void enableP2p(bool on);

    void clearError();

    // ---- state ----
    // Copies the state into `out` if it changed since `out` was filled (out.serial), and refreshes
    // the live network fields either way. Returns whether anything but those changed.
    bool snapshot(Snapshot& out) const;
    Snapshot snapshot() const;

    // The network as a LAN port (installed automatically when installLanPort).
    lan::Port& port();

    struct Impl;

private:
    friend void detail::simulateConnectionLoss(Client&);
    friend void detail::setUdpGateway(Client&, const std::string&);
    friend void detail::setMacAddress(Client&, const uint8_t*);
    std::unique_ptr<Impl> impl_;
};

// The game's client (installs the LAN port). Created on first use.
Client& client();
// At exit: leaves the lobby, closes the connection, stops the thread.
void shutdown();

// ---- internals, exposed for tests ----
namespace detail {
void sha256(const void* data, size_t len, uint8_t out[32]);
void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]);
bool scrypt(const void* password, size_t passwordLen, const void* salt, size_t saltLen, uint64_t n,
            uint32_t r, uint32_t p, uint8_t* out, size_t outLen);
// (Tests) all this client's UDP goes through a NAT simulator at hostPort, wrapped: 4-byte IPv4 +
// 2-byte port (big-endian) of the far end, then the datagram, both ways.
void setUdpGateway(Client& client, const std::string& hostPort);
// (Tests) this client's console MAC (else rt::lan::macAddress()).
void setMacAddress(Client& client, const uint8_t mac[6]);
}  // namespace detail

}  // namespace rt::airnet
