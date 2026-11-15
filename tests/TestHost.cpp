// End-to-end test of the real plugin against a simulated Script Hook V.
//
//   TestHost.exe <build folder> <repo root>
//
// It loads the real ChatVsStreamerGTAV.asi and the mock ScriptHookV.dll, then drives
// them the way the game would: script frames on a fiber, plus real HTTP requests
// and chat lines. Time runs 30x faster so two-minute timeouts take seconds.
//
// Other modes:
//   --rph                      runs the same tests against the RAGE Plugin Hook build
//                              (ChatVsStreamerCore.dll), with this program standing in for
//                              its C# plugin, plus some checks only that build needs
//   --twitch-smoke <channel>   connects to real Twitch chat anonymously (read-only,
//                              nothing is ever posted) to check the WebSocket path
//   --serve                    keeps the simulated game running at normal speed, so
//                              the panel and overlay can be tried in a browser

#include "../src/Util.h"
#include "../src/Json.h"
#define CVS_SHV_API
#include "../src/shv/Natives.h"
#include "../src/rph/RphApi.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace cvs;
namespace H = cvs::nat::hash;

namespace cvs
{
    std::filesystem::path DataDir() { return {}; } // required by Util.h; unused here
}

namespace
{
    // Demo mode has its own port, because a test run leaves thousands of closed
    // connections on the test port for a few minutes afterwards.
    int g_port = 17373;
    constexpr int kRphPort = 17374;
    constexpr int kServePort = 17380;
    constexpr double kTimeScale = 30.0;

    int g_failures = 0, g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        ++g_checks;
        if (!ok) { ++g_failures; printf("  FAIL  %s\n", what.c_str()); }
    }

    void CheckEq(long long actual, long long expected, const std::string& what)
    {
        Check(actual == expected, what + " (got " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
    }

    void Section(const char* name) { printf("%s\n", name); fflush(stdout); }

    // ── The mock's controls ─────────────────────────────────────────────────────

    struct Mock
    {
        void (*init)();
        void (*frame)();
        void (*restart)();
        void (*set)(const char*, int);
        int (*get)(const char*);
        void (*clearCalls)();
        void (*clearModels)();
        int (*count)(uint64_t);
        int (*totalCalls)();
        int (*lastInt)(uint64_t, int, int*);
        int (*lastFloat)(uint64_t, int, float*);
        int (*nthFloat)(uint64_t, int, int, float*);
        float (*fedControl)(int);
        int (*lastText)(uint64_t, char*, int);
        int (*countActions)();
        void (*key)(DWORD);
        int (*countArg)(uint64_t, int, int);
    } mock;

    template <class F>
    void Bind(HMODULE m, F& fn, const char* name)
    {
        fn = reinterpret_cast<F>(GetProcAddress(m, name));
        if (!fn) { printf("A DLL is missing %s\n", name); ExitProcess(2); }
    }

    // ── Standing in for the C# plugin (--rph) ───────────────────────────────────

    struct RphCore
    {
        int32_t (*start)(int32_t, const CvsRphHost*, const char*);
        void (*tick)();
        void (*stop)();
    } rph;

    PUINT64 (*g_mockCall)(uint64_t, const uint64_t*, int) = nullptr;
    int (*g_mockVehicles)(int*, int) = nullptr;
    int (*g_mockPeds)(int*, int) = nullptr;
    int g_rphKeyDown = 0;

    int32_t RphCallInt(uint64_t hash, const uint64_t* args, int32_t count) { return *reinterpret_cast<int32_t*>(g_mockCall(hash, args, count)); }
    float RphCallFloat(uint64_t hash, const uint64_t* args, int32_t count) { return *reinterpret_cast<float*>(g_mockCall(hash, args, count)); }

    void RphCallVector(uint64_t hash, const uint64_t* args, int32_t count, float* xyz)
    {
        const Vector3 v = *reinterpret_cast<Vector3*>(g_mockCall(hash, args, count));
        xyz[0] = v.x;
        xyz[1] = v.y;
        xyz[2] = v.z;
    }

    int32_t RphVehicles(int32_t* handles, int32_t capacity) { return g_mockVehicles(handles, capacity); }
    int32_t RphPeds(int32_t* handles, int32_t capacity) { return g_mockPeds(handles, capacity); }
    int32_t RphIsKeyDown(int32_t key) { return key == g_rphKeyDown; }

    const CvsRphHost kRphHost{ RphCallInt, RphCallFloat, RphCallVector, RphVehicles, RphPeds, RphIsKeyDown };

    // These replace the mock's versions, so every test drives the build it's testing the same way.
    void RphFrame()
    {
        rph.tick();
        g_rphKeyDown = 0; // a press lasts one frame unless a test holds the key itself
    }
    void RphReload()
    {
        rph.stop();
        rph.start(kCvsRphApiVersion, &kRphHost, "test");
    }
    void RphKey(DWORD key) { g_rphKeyDown = (int)key; }

    // Exit code 3 means the tests never ran, as opposed to running and failing.
    int ReportBlockedOrFailedLoad(const char* file, DWORD error)
    {
        constexpr DWORD kBlockedByAppControl = 4551; // ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION
        if (error == kBlockedByAppControl)
        {
            printf("Windows blocked %s from loading. This is usually Smart App Control, which can refuse a freshly built, unsigned file.\n", file);
            return 3;
        }
        printf("%s failed to load (%lu). If this says a procedure wasn't found, ScriptHookV.def and ScriptHookV.h disagree.\n", file, error);
        return 2;
    }

    bool ImportsDll(HMODULE module, const char* name)
    {
        auto* base = reinterpret_cast<const BYTE*>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; import->Name; ++import)
            if (_stricmp(reinterpret_cast<const char*>(base + import->Name), name) == 0) return true;
        return false;
    }

    // ── HTTP and SSE ────────────────────────────────────────────────────────────

    struct Response { int status = 0; std::string body; };

    SOCKET ConnectLocal()
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)g_port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) { closesocket(s); return INVALID_SOCKET; }
        return s;
    }

    Response Http(const std::string& method, const std::string& path, const std::string& body = "",
                  const std::string& extraHeaders = "X-CVS-Panel: 1\r\n", const std::string& host = "")
    {
        Response r;
        SOCKET s = ConnectLocal();
        if (s == INVALID_SOCKET) return r;
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? "localhost:" + std::to_string(g_port) : host) + "\r\n" + extraHeaders +
                          "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(s, req.data(), (int)req.size(), 0);
        std::string data;
        char buf[8192];
        int n;
        while ((n = recv(s, buf, sizeof(buf), 0)) > 0) data.append(buf, n);
        closesocket(s);
        if (data.size() > 12) r.status = atoi(data.c_str() + 9);
        size_t split = data.find("\r\n\r\n");
        if (split != std::string::npos) r.body = data.substr(split + 4);
        return r;
    }

    std::string UrlEncode(const std::string& s)
    {
        std::string out;
        char buf[8];
        for (unsigned char c : s)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out.push_back((char)c);
            else { snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
        }
        return out;
    }

    class EventStream
    {
    public:
        void Start()
        {
            m_socket = ConnectLocal();
            std::string req = "GET /events HTTP/1.1\r\nHost: localhost:" + std::to_string(g_port) + "\r\n\r\n";
            send(m_socket, req.data(), (int)req.size(), 0);
            m_thread = std::thread([this]
            {
                char buf[4096];
                int n;
                while ((n = recv(m_socket, buf, sizeof(buf), 0)) > 0)
                {
                    std::lock_guard lock(m_mutex);
                    m_data.append(buf, n);
                }
            });
            m_thread.detach();
        }
        std::string Text()
        {
            std::lock_guard lock(m_mutex);
            return m_data;
        }
        void Clear()
        {
            std::lock_guard lock(m_mutex);
            m_data.clear();
        }
        bool Contains(const std::string& needle) { return Text().find(needle) != std::string::npos; }

    private:
        SOCKET m_socket = INVALID_SOCKET;
        std::thread m_thread;
        std::mutex m_mutex;
        std::string m_data;
    } events;

    // ── Driving the game ────────────────────────────────────────────────────────

    // Runs script frames until the condition holds, or the (real-time) limit passes.
    bool Pump(const std::function<bool()>& until, double wallSeconds = 8.0)
    {
        const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(wallSeconds * 1000);
        while (GetTickCount64() < deadline)
        {
            mock.frame();
            if (until()) return true;
            Sleep(3);
        }
        return false;
    }

    void PumpFor(double gameSeconds)
    {
        const ULONGLONG until = GetTickCount64() + (ULONGLONG)(gameSeconds / kTimeScale * 1000);
        while (GetTickCount64() < until)
        {
            mock.frame();
            Sleep(3);
        }
    }

    void Chat(const std::string& login, const std::string& text, const std::string& role = "")
    {
        std::string badges = role == "mod" ? "moderator/1" : role == "sub" ? "subscriber/1000" : "";
        std::string line = "@badges=" + badges + ";display-name=" + login + ";mod=" + (role == "mod" ? "1" : "0") +
                           " :" + login + "!" + login + "@" + login + ".tmi.twitch.tv PRIVMSG #test :" + text;
        Http("POST", "/api/simulate", "line=" + UrlEncode(line));
    }

    long long Balance(const std::string& login)
    {
        events.Clear();
        Chat(login, "!balance");
        const std::string tag = "@" + login + " you have ";
        std::string text;
        if (!Pump([&] { text = events.Text(); return text.find(tag) != std::string::npos; })) return -1;
        return atoll(text.c_str() + text.find(tag) + tag.size());
    }

    void Fire(const std::string& id) { Http("POST", "/api/fire", "id=" + UrlEncode(id)); }

    bool WaitIdle()
    {
        return Pump([] { auto b = Http("GET", "/state").body; return b.find("\"queued\":0") != std::string::npos && b.find("\"active\":[]") != std::string::npos; }, 15.0);
    }

    bool WaitFired(const std::string& id, double wall = 6.0)
    {
        return Pump([&] { return events.Contains("\"type\":\"fired\",\"effect\":\"" + id + "\""); }, wall);
    }

    void ResetState()
    {
        for (const char* k : { "dead", "noControl", "arrested", "switching", "wanted", "mission", "cutscene", "loading",
                               "paused", "online", "inVehicle", "spawnFails", "modelMissing", "unarmed", "modelLoadRequests",
                               "entitiesGone", "ragdolling", "collided", "floorHit", "ceilingHit", "wallAhead", "camYaw", "stopped", "hornHeld",
                               "convertible", "noDrivers", "pedsShooting", "impactZ", "ownsWeapon", "inWater",
                               "othersDead", "missingModel", "missingWeapon", "onlyModel", "playerPed", "inAir", "swimming",
                               "parachuteState", "pedsHostile", "raisedBy", "bike", "othersHurt", "invincible",
                               "held32", "held33", "held34", "held35", "held63", "held64" })
            mock.set(k, 0);
        mock.set("cash", 10000);
    }

    // Has the game clean up everything spawned so far, so the cap of 40 doesn't hold up
    // whatever is tested next. The cap itself is tested in TestSpawning.
    void ClearSpawned()
    {
        mock.set("entitiesGone", 1);
        PumpFor(2);
        mock.set("entitiesGone", 0);
    }

    // ── Pure functions ──────────────────────────────────────────────────────────

    void TestHashes()
    {
        Section("Name hashes");
        Check(Joaat("adder") == 0xB779A091, "joaat adder");
        Check(Joaat("player_zero") == 0x0D7114C9, "joaat player_zero");
        Check(Joaat("player_one") == 0x9B22DBAF, "joaat player_one");
        Check(Joaat("player_two") == 0x9B810FA2, "joaat player_two");
        Check(Joaat("SP0_TOTAL_CASH") == 0x0324C31D, "joaat SP0_TOTAL_CASH (case-insensitive)");
        Check(Joaat("weapon_unarmed") == 0xA2719263, "joaat weapon_unarmed");
        Check(Joaat("weapon_pistol") == 0x1B06D571, "joaat weapon_pistol");
        Check(Joaat("weapon_carbinerifle") == 0x83BF0278, "joaat weapon_carbinerifle");
        Check(Joaat("gadget_parachute") == 0xFBAB5776, "joaat gadget_parachute");
        Check(Joaat("PLAYER") == 0x6F0783F5, "joaat PLAYER relationship group");
    }

    void TestText()
    {
        Section("Text cleanup");
        Check(FeedSafe("a~r~b") == "arb", "FeedSafe strips formatting tokens");
        Check(FeedSafe(std::string(100, 'x')).size() == 40, "FeedSafe truncates");
        Check(FeedSafe("\xE2\x82\xAC\xE2\x82\xAC", 4) == "\xE2\x82\xAC", "FeedSafe never splits a UTF-8 character");
        Check(PlateText("some_long_name!") == "SOMELONG", "PlateText is 8 uppercase alphanumerics");
        Check(PlateText("___") == "CHAOS", "PlateText has a fallback");
        Check(IsValidLogin("good_name1"), "IsValidLogin accepts a normal login");
        Check(!IsValidLogin("bad\tname") && !IsValidLogin("") && !IsValidLogin(std::string(26, 'a')), "IsValidLogin rejects junk");
        Check(ToInt("1e300", 7) == 2000000000 && ToInt("nan", 7) == 7 && ToInt("abc", 7) == 7, "ToInt survives hostile numbers");
    }

    void TestJson()
    {
        Section("Reading JSON");
        auto doc = Json::Parse(R"({"a":{"b":[1,2.5,-3e2,"x"],"t":true,"n":null},"s":"q\"\\\/\né😀"})");
        Check(doc.has_value(), "parses a nested document");
        if (doc)
        {
            const Json& d = *doc;
            Check(d["a"]["b"].Size() == 4 && d["a"]["b"].At(1).Num() == 2.5 && d["a"]["b"].At(2).Num() == -300, "numbers and arrays");
            Check(d["a"]["t"].Bool() && d["a"]["n"].IsNull(), "true and null");
            Check(d["s"].Str() == "q\"\\/\n\xC3\xA9\xF0\x9F\x98\x80", "escapes, including an emoji sent as two halves");
            Check(d["missing"]["deeper"].Str("fallback") == "fallback" && d["a"]["b"].At(99).IsNull(), "a missing key or index is null, not a crash");
            Check(d["a"]["b"].Str("fallback") == "fallback" && d["s"].Num(7) == 7, "a value of the wrong type gives the fallback");
        }
        Check(!Json::Parse(R"({"a":1)") && !Json::Parse(R"({"a":"unterminated})") && !Json::Parse("[1,]") &&
              !Json::Parse("{} junk") && !Json::Parse("") && !Json::Parse(R"("\ud83d")") && !Json::Parse("\"raw\ttab\""),
              "malformed input is refused");
        Check(!Json::Parse(std::string(100000, '[')), "deep nesting is refused, without overflowing the stack");
    }

    // ── Web server ──────────────────────────────────────────────────────────────

    void TestWebSecurity()
    {
        Section("Web server only answers to itself");
        Check(Http("GET", "/").status == 200, "overlay is served");
        Check(Http("GET", "/panel").status == 200, "panel is served");
        Check(Http("GET", "/state").status == 200, "state is served");
        Check(Http("GET", "/state", "", "", "evil.example.com").status == 403, "wrong Host is refused (DNS rebinding)");
        Check(Http("GET", "/nope").status == 404, "unknown path is a 404");

        const std::string body = "key=P_drunk&value=1";
        Check(Http("POST", "/api/set", body, "").status == 403, "POST without the panel header is refused");
        Check(Http("POST", "/api/set", body, "X-CVS-Panel: 1\r\nOrigin: https://evil.example.com\r\n").status == 403, "POST from another site is refused");
        Check(Http("POST", "/api/set", body, "X-CVS-Panel: 1\r\nOrigin: http://localhost:" + std::to_string(g_port) + "\r\n").status == 200, "POST from the panel's own origin works");
        Http("POST", "/api/set", "key=P_drunk&value=200");

        auto state = Http("GET", "/state").body;
        auto settings = Http("GET", "/api/settings").body;
        Check(state.find("oauth") == std::string::npos && settings.find("oauth") == std::string::npos &&
              settings.find("SECRET") == std::string::npos, "no endpoint exposes the Twitch token");
    }

    void TestSettings(const fs::path& data)
    {
        Section("Settings");
        Check(Http("POST", "/api/set", "key=NotASetting&value=1").status == 400, "unknown setting is refused");
        Check(Http("POST", "/api/set", "key=P_notaneffect&value=1").status == 400, "price for an unknown effect is refused");

        Http("POST", "/api/set", "key=P_fog&value=123");
        Check(Http("GET", "/api/settings").body.find("\"id\":\"fog\",\"name\":\"Pea Soup\",\"tier\":\"Nuisance\",\"description\":\"Thick fog rolls in.\",\"defaultPrice\":100,\"price\":123") != std::string::npos,
              "price change shows up");
        std::ifstream in(data / L"settings.ini");
        std::stringstream text;
        text << in.rdbuf();
        Check(text.str().find("P_fog=123") != std::string::npos, "price change is saved to settings.ini");

        Http("POST", "/api/set", "key=P_fog&value=-50");
        Check(Http("GET", "/state").body.find("\"id\":\"fog\",\"name\":\"Pea Soup\",\"price\":1,") != std::string::npos, "prices can't go below 1");
        Http("POST", "/api/set", "key=P_fog&value=100");

        // A hand edit should apply without a restart.
        std::string edited = text.str();
        size_t at = edited.find("P_fog=123");
        if (at != std::string::npos) edited.replace(at, 9, "P_fog=777");
        Sleep(1100); // so the file's timestamp visibly changes
        std::ofstream(data / L"settings.ini", std::ios::trunc | std::ios::binary) << edited;
        bool reloaded = Pump([] { return Http("GET", "/state").body.find("\"id\":\"fog\",\"name\":\"Pea Soup\",\"price\":777,") != std::string::npos; }, 4.0);
        Check(reloaded, "a hand edit of settings.ini applies live");
        Http("POST", "/api/set", "key=P_fog&value=100");
    }

    // ── Economy ─────────────────────────────────────────────────────────────────

    void TestEconomy(const fs::path& data)
    {
        Section("Points");
        Check(Http("GET", "/state").body.find("seeded_viewer") != std::string::npos ||
              Http("GET", "/state").body.find("SeededViewer") != std::string::npos, "the ledger loads from disk at startup");

        // 100 starting points + 5 for chatting.
        Check(Balance("alice") == 105, "a new viewer gets starting points plus the chat bonus");

        events.Clear();
        Chat("alice", "!buy fog");
        Check(WaitFired("fog"), "!buy fires the effect");
        char weather[64]{};
        mock.lastText(H::SET_WEATHER_TYPE_NOW_PERSIST, weather, sizeof(weather));
        Check(std::string(weather) == "FOGGY", "fog sets foggy weather");
        Check(Balance("alice") == 5, "the price is deducted");

        events.Clear();
        Chat("alice", "!buy tank");
        Check(Pump([] { return events.Contains("costs 3000 and you have 5"); }), "can't buy what you can't afford");
        Check(Balance("alice") == 5, "a refused purchase costs nothing");

        events.Clear();
        Chat("bob", "!buy notathing");
        Check(Pump([] { return events.Contains("no effect called 'notathing'"); }), "unknown effect gets a reply");

        Chat("mod_user", "!give dave 500", "mod");
        Check(Balance("dave") == 605, "mods can mint points");

        Chat("dave", "!give erin 100");
        Check(Balance("dave") == 505, "viewers transfer their own points");
        Check(Balance("erin") == 205, "transfer arrives");

        events.Clear();
        Chat("dave", "!give bad~name 5");
        Check(Pump([] { return events.Contains("isn't a valid username"); }), "!give rejects junk names");
        Chat("dave", "!give erin 999999");
        Check(Balance("dave") == 505, "can't give more than you have");
        Chat("dave", "!give erin -50");
        Check(Balance("dave") == 505, "can't give a negative amount");

        // Bits, a sub, a gift and a raid.
        Http("POST", "/api/simulate", "line=" + UrlEncode("@badges=;bits=50;display-name=frank :frank!frank@frank.tmi.twitch.tv PRIVMSG #test :Cheer50 nice"));
        Check(Balance("frank") == 155, "bits convert to points");
        Http("POST", "/api/simulate", "line=" + UrlEncode("@display-name=gina;login=gina;msg-id=sub;msg-param-sub-plan=1000 :tmi.twitch.tv USERNOTICE #test"));
        Check(Balance("gina") == 205, "a sub pays the sub bonus");

        // Saves within the worker's interval (30s of accelerated time).
        Chat("alice", "hello");
        bool saved = Pump([&]
        {
            std::ifstream in(data / L"ledger.tsv");
            std::stringstream text;
            text << in.rdbuf();
            return text.str().find("dave\tdave\t505") != std::string::npos;
        });
        Check(saved, "the ledger is saved to disk");

        Section("Leaderboard");
        {
            std::ifstream in(data / L"leaderboard_hidden.txt");
            std::stringstream text;
            text << in.rdbuf();
            Check(text.str().find("\nstreamelements\n") != std::string::npos, "the mod creates leaderboard_hidden.txt with common chat bots");
        }
        // The test ini names the channel TestStreamer and the mod's bot ChatHelper, in capitals,
        // which the mod has to lower to match the logins chat uses. Everyone hidden gets more
        // points than top_fan.
        Chat("mod_user", "!give teststreamer 900000", "mod");
        Chat("mod_user", "!give chathelper 800000", "mod");
        Chat("mod_user", "!give quizbot_fan 750000", "mod");
        Chat("mod_user", "!give streamelements 720000", "mod");
        Chat("mod_user", "!give top_fan 700000", "mod");
        const bool listed = Pump([] { return Http("GET", "/state").body.find("\"name\":\"top_fan\",\"points\":700100") != std::string::npos; });
        Check(listed, "a viewer is listed");
        std::string board = Http("GET", "/state").body;
        const size_t start = board.find("\"leaderboard\":[");
        Check(start != std::string::npos && board.find("\"name\":\"top_fan\"", start) == start + 16, "first, above the accounts with more points");
        Check(board.find("teststreamer") == std::string::npos, "the streamer's own account is left off");
        Check(board.find("chathelper") == std::string::npos, "so is the mod's bot account");
        Check(board.find("quizbot_fan") == std::string::npos, "so is any name with \"bot\" in it");
        Check(board.find("streamelements") == std::string::npos, "so are the accounts in leaderboard_hidden.txt");

        std::ofstream(data / L"leaderboard_hidden.txt", std::ios::binary | std::ios::trunc) << "# edited\n\nTop_Fan\n";
        Check(Pump([] { return Http("GET", "/state").body.find("top_fan") == std::string::npos; }), "a name added to the file drops off within seconds");
        Check(Http("GET", "/state").body.find("streamelements") != std::string::npos, "and one taken out comes back");

        CheckEq(Balance("teststreamer"), 900105LL, "hidden accounts keep their points");
        Check(Http("GET", "/state").body.find("teststreamer") == std::string::npos, "and chatting doesn't put them back on");
    }

    // A redemption as Twitch EventSub sends it.
    void Redeem(const std::string& id, const std::string& login, const std::string& title)
    {
        const std::string message = std::format(
            R"({{"metadata":{{"message_id":"msg-{}","message_type":"notification","message_timestamp":"2026-11-08T18:00:00.0Z",)"
            R"("subscription_type":"channel.channel_points_custom_reward_redemption.add","subscription_version":"1"}},)"
            R"("payload":{{"subscription":{{"id":"sub-1","status":"enabled","type":"channel.channel_points_custom_reward_redemption.add","version":"1"}},)"
            R"("event":{{"id":"{}","broadcaster_user_id":"1","broadcaster_user_login":"teststreamer","broadcaster_user_name":"TestStreamer",)"
            R"("user_id":"2","user_login":"{}","user_name":"{}","user_input":"","status":"unfulfilled",)"
            R"("reward":{{"id":"reward-1","title":"{}","cost":500,"prompt":""}},"redeemed_at":"2026-11-08T18:00:00.0Z"}}}}}})",
            id, id, login, login, title);
        Http("POST", "/api/simulate-eventsub", "message=" + UrlEncode(message));
    }

    void TestRewards(const fs::path& data)
    {
        Section("Channel point rewards");
        {
            std::ifstream in(data / L"channel_point_rewards.txt");
            std::stringstream text;
            text << in.rdbuf();
            Check(text.str().find("\nGet Chaos Points (Small) = 50\nGet Chaos Points (Medium) = 150\nGet Chaos Points (Large) = 400\n") != std::string::npos,
                  "the mod creates channel_point_rewards.txt with the Stardew version's three rewards");
        }
        Check(Http("GET", "/state").body.find("\"channelPoints\":false") != std::string::npos, "without BroadcasterToken nothing connects to Twitch");

        // So asking for a balance doesn't change it, and no passive payout lands mid-test.
        Http("POST", "/api/set", "key=ChatBonus&value=0");
        Http("POST", "/api/set", "key=PassivePoints&value=0");

        events.Clear();
        Redeem("red-1", "reward_fan", "Get Chaos Points (Large)");
        Check(Pump([] { return events.Contains("@reward_fan redeemed Get Chaos Points (Large) and received 400 chaos points! (balance: 500pts)"); }),
              "a listed reward pays its points, with a reply worded like Stardew's");
        Check(events.Contains("\"type\":\"points\",\"viewer\":\"reward_fan\",\"text\":\"redeemed Get Chaos Points (Large) (+400 pts)\""),
              "and the overlay gets a toast");

        Redeem("red-1", "reward_fan", "Get Chaos Points (Large)");
        Redeem("red-2", "reward_fan", "get chaos points (SMALL)");
        CheckEq(Balance("reward_fan"), 550LL, "capitals don't matter, and the same redemption delivered twice pays once");

        events.Clear();
        Redeem("red-3", "reward_fan", "Hydrate!");
        Redeem("red-4", "bad~name", "Get Chaos Points (Large)");
        Http("POST", "/api/simulate-eventsub", "message=" + UrlEncode(R"({"metadata":{"message_type":"session_keepalive"},"payload":{}})"));
        Http("POST", "/api/simulate-eventsub", "message=" + UrlEncode(R"({"metadata":{"message_type":"session_welcome"},"payload":{"session":{"id":"x"}}})"));
        Http("POST", "/api/simulate-eventsub", "message=" + UrlEncode("{\"metadata\":"));
        CheckEq(Balance("reward_fan"), 550LL, "other rewards, junk names and other messages change nothing");
        Check(!events.Contains("Hydrate!") && !events.Contains("bad~name"), "and get no reply");

        // A hand edit applies without a restart. Hydrate! pays nothing until the file is read again.
        Sleep(1100); // so the file's timestamp visibly changes
        std::ofstream(data / L"channel_point_rewards.txt", std::ios::binary | std::ios::trunc)
            << "\xEF\xBB\xBF# edited in Notepad\nGet Chaos Points (Small) = 75\nHydrate! = 10\nBroken line\nZero = 0\n";
        bool reloaded = false;
        for (int i = 0; i < 20 && !reloaded; ++i)
        {
            Redeem("hydrate-" + std::to_string(i), "hydrator", "Hydrate!");
            reloaded = Pump([] { return events.Contains("@hydrator redeemed Hydrate! and received 10 chaos points!"); }, 0.3);
        }
        Check(reloaded, "a hand edit of channel_point_rewards.txt applies within seconds");
        Redeem("red-5", "reward_fan", "Get Chaos Points (Small)");
        Check(Pump([] { return events.Contains("@reward_fan redeemed Get Chaos Points (Small) and received 75 chaos points! (balance: 625pts)"); }),
              "with the new amount");
        events.Clear();
        Redeem("red-6", "reward_fan", "Get Chaos Points (Large)");
        CheckEq(Balance("reward_fan"), 625LL, "and a reward taken out of the file pays nothing");

        Http("POST", "/api/set", "key=ChatBonus&value=5");
        Http("POST", "/api/set", "key=PassivePoints&value=10");
    }

    // ── The guard rails ─────────────────────────────────────────────────────────

    void TestGating()
    {
        Section("Effects wait for a safe moment");
        Chat("mod_user", "!give hal 500000", "mod");
        Balance("hal");

        const std::pair<const char*, const char*> blocks[] = {
            { "loading", "the game is loading" }, { "cutscene", "a cutscene is playing" }, { "dead", "wasted" },
            { "arrested", "busted" }, { "switching", "switching" }, { "paused", "paused" }, { "noControl", "control" },
            { "online", "story mode" },
        };
        for (auto& [key, reason] : blocks)
        {
            ResetState();
            mock.set(key, 1);
            PumpFor(1);
            mock.clearCalls();
            events.Clear();
            Fire("storm");
            PumpFor(6);
            Check(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST) == 0, std::string("nothing fires while ") + key);
            Check(Http("GET", "/state").body.find(reason) != std::string::npos, std::string("state explains ") + key);
            mock.set(key, 0);
            Check(WaitFired("storm"), std::string("it fires once ") + key + " ends");
            WaitIdle();
        }

        // In story mode, online is the one that matters most: nothing but polling, at all.
        ResetState();
        mock.set("online", 1);
        PumpFor(1);
        mock.clearCalls();
        Fire("tank");
        Fire("nuclear");
        PumpFor(10);
        Check(mock.count(H::CREATE_VEHICLE) == 0 && mock.count(H::ADD_EXPLOSION) == 0 && mock.count(H::REQUEST_MODEL) == 0,
              "online: effects never touch the game");
        Http("POST", "/api/control", "action=clear");
        ResetState();

        Section("Vehicle and on-foot effects");
        events.Clear();
        mock.clearCalls();
        Fire("flat");
        PumpFor(5);
        Check(mock.count(H::SET_VEHICLE_TYRE_BURST) == 0, "a vehicle effect waits while on foot");
        mock.set("inVehicle", 1);
        Check(WaitFired("flat"), "and fires on entering a vehicle");
        Check(mock.count(H::SET_VEHICLE_TYRE_BURST) == 6, "all six wheel slots are popped");
        WaitIdle();

        events.Clear();
        mock.clearCalls();
        Fire("butterfingers");
        PumpFor(5);
        Check(mock.count(H::SET_PED_DROPS_INVENTORY_WEAPON) == 0, "an on-foot effect waits while in a vehicle");
        mock.set("inVehicle", 0);
        Check(WaitFired("butterfingers"), "and fires on getting out");
        WaitIdle();

        Section("Risky effects wait for missions");
        ResetState();
        mock.set("mission", 1);
        events.Clear();
        mock.clearCalls();
        Fire("warp");
        PumpFor(6);
        Check(mock.count(H::SET_ENTITY_COORDS) == 0, "no teleport during a mission");
        mock.set("mission", 0);
        Check(WaitFired("warp"), "teleport runs when the mission ends");
        Check(mock.count(H::SET_ENTITY_COORDS) == 1, "exactly one teleport");

        Http("POST", "/api/set", "key=BlockRiskyInMissions&value=0");
        mock.set("mission", 1);
        events.Clear();
        Fire("midnight");
        Check(WaitFired("midnight"), "the mission hold can be turned off");
        Http("POST", "/api/set", "key=BlockRiskyInMissions&value=1");
        ResetState();
        WaitIdle();

        Section("Refunds");
        // Each !balance below would otherwise earn points, and so would a passive payout landing mid-test.
        Http("POST", "/api/set", "key=ChatBonus&value=0");
        Http("POST", "/api/set", "key=PassivePoints&value=0");
        events.Clear();
        Chat("ivan", "!give ivan 1000", "mod");
        long long before = Balance("ivan");
        events.Clear();
        Chat("ivan", "!buy flat");
        PumpFor(20);
        CheckEq(Balance("ivan"), before - 250, "points are held while it waits");
        events.Clear();
        Pump([] { return events.Contains("refunded"); }, 6.0);
        Check(events.Contains("Flat Tires couldn't run (the streamer stayed on foot), so 250 points were refunded"), "times out with the real reason");
        CheckEq(Balance("ivan"), before, "the refund arrives");

        // An effect the game refuses outright refunds straight away.
        mock.set("wanted", 0);
        events.Clear();
        Chat("ivan", "!buy clean");
        Check(Pump([] { return events.Contains("isn't wanted right now), so 400 points were refunded"); }), "a refused effect refunds at once");
        CheckEq(Balance("ivan"), before, "with all points back");
        Http("POST", "/api/set", "key=ChatBonus&value=5");
        Http("POST", "/api/set", "key=PassivePoints&value=10");

        // A backlog after a long gap gets refunded, not fired all at once.
        ResetState();
        Http("POST", "/api/set", "key=Enabled&value=1");
        mock.set("paused", 1);
        PumpFor(1);
        events.Clear();
        mock.clearCalls();
        Fire("storm");
        Fire("snow");
        PumpFor(125);
        mock.set("paused", 0);
        PumpFor(5);
        Check(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST) == 0, "a stale backlog doesn't fire late");
        ResetState();
    }

    void TestSpawning()
    {
        Section("Spawning");
        ResetState();
        mock.set("modelLoadRequests", 3);
        mock.clearModels();
        mock.clearCalls();
        events.Clear();
        Fire("dogs");
        Check(WaitFired("dogs"), "waits for the model to stream in, then spawns");
        Check(mock.count(H::REQUEST_MODEL) >= 3, "the model was requested until loaded");
        Check(mock.count(H::CREATE_PED) == 3, "three dogs");
        Check(mock.count(H::TASK_COMBAT_PED) == 3, "all attacking");
        Check(mock.count(H::SET_MODEL_AS_NO_LONGER_NEEDED) >= 1, "the model is released");
        int seat = 0, pedType = 0;
        mock.lastInt(H::CREATE_PED, 0, &pedType);
        Check(pedType == 28, "dogs use the animal ped type");
        (void)seat;

        ResetState();
        mock.set("modelMissing", 1);
        events.Clear();
        Fire("clowns");
        Check(Pump([] { return events.Contains("doesn't have that model"); }), "a missing model fails cleanly instead of hanging");
        ResetState();

        mock.set("spawnFails", 1);
        events.Clear();
        Fire("zombies");
        Check(Pump([] { return events.Contains("wouldn't spawn them"); }), "a failed spawn is reported");
        ResetState();

        events.Clear();
        mock.clearCalls();
        Http("POST", "/api/simulate", "line=" + UrlEncode("@display-name=raider;login=raider;msg-id=raid;msg-param-viewerCount=30 :tmi.twitch.tv USERNOTICE #test"));
        Check(WaitFired("mob"), "a raid sends a mob");
        Check(mock.count(H::CREATE_PED) == 6, "30 raiders make 6 attackers");
        events.Clear();
        mock.clearCalls();
        Http("POST", "/api/simulate", "line=" + UrlEncode("@display-name=raider;login=raider;msg-id=raid;msg-param-viewerCount=1000 :tmi.twitch.tv USERNOTICE #test"));
        Check(WaitFired("mob"), "a huge raid sends a mob");
        Check(mock.count(H::CREATE_PED) == 12, "a huge raid is capped at 12");

        // Spawn limit: everything alive is capped so the game's pools can't fill up.
        PumpFor(200); // the earlier attackers time out and are released
        Check(mock.count(H::SET_ENTITY_AS_NO_LONGER_NEEDED) >= 12, "spawned entities are handed back to the game");

        events.Clear();
        mock.clearCalls();
        for (int i = 0; i < 4; ++i) Http("POST", "/api/simulate", "line=" + UrlEncode("@display-name=raider;login=raider;msg-id=raid;msg-param-viewerCount=60 :tmi.twitch.tv USERNOTICE #test"));
        PumpFor(8);
        Check(mock.count(H::CREATE_PED) == 36, "three mobs of 12 fit under the cap of 40");
        Http("POST", "/api/control", "action=clear");
        PumpFor(200);
    }

    // ── Timed effects always restore ────────────────────────────────────────────

    void TestRestores()
    {
        Section("Timed effects put everything back");
        ResetState();

        struct Case { const char* id; bool inVehicle; std::function<bool()> restored; const char* what; };
        const std::vector<Case> cases = {
            { "moon", false, [] { int v = -1; return mock.lastInt(H::SET_GRAVITY_LEVEL, 0, &v) && v == 0; }, "gravity back to normal" },
            { "slowmo", false, [] { float v = 0; return mock.lastFloat(H::SET_TIME_SCALE, 0, &v) && v == 1.0f; }, "time scale back to 1" },
            { "blackout", false, [] { int v = -1; return mock.lastInt(H::SET_ARTIFICIAL_LIGHTS_STATE, 0, &v) && v == 0; }, "lights back on" },
            { "plotarmor", false, [] { int v = -1; return mock.lastInt(H::SET_PLAYER_INVINCIBLE, 1, &v) && v == 0; }, "invincibility off" },
            { "caffeine", false, [] { float v = 0; return mock.lastFloat(H::SET_RUN_SPRINT_MULTIPLIER_FOR_PLAYER, 1, &v) && v == 1.0f; }, "run speed back to 1" },
            { "drunk", false, [] { int v = -1; return mock.count(H::RESET_PED_MOVEMENT_CLIPSET) == 1 && mock.lastInt(H::SET_PED_IS_DRUNK, 1, &v) && v == 0 && mock.count(H::STOP_GAMEPLAY_CAM_SHAKING) == 1 && mock.count(H::ANIMPOSTFX_STOP) == 1; }, "walk, speech, camera and screen back to normal" },
            { "ice", true, [] { int v = -1; return mock.lastInt(H::SET_VEHICLE_REDUCE_GRIP, 1, &v) && v == 0; }, "grip back to normal" },
            { "honk", true, [] { int v = -1; return mock.lastInt(H::SET_ENTITY_PROOFS, 3, &v) && v == 0; }, "explosions hurt again" },
            { "loadout", false, [] { int v = -1; return mock.lastInt(H::SET_PED_CAN_SWITCH_WEAPON, 1, &v) && v == 1; }, "weapon switching back on" },
            { "flood", false, [] { float v = 0; return mock.count(H::RESET_DEEP_OCEAN_SCALER) == 1 && mock.lastFloat(H::SET_RAIN, 0, &v) && v == -1.0f; }, "waves and rain back to normal" },
            { "madmax", false, [] { float v = -1; return mock.count(H::CLEAR_PED_TASKS) == 2 && mock.lastFloat(H::MODIFY_VEHICLE_TOP_SPEED, 1, &v) && v == 0.0f; }, "drivers released and top speed back to normal" },
            { "horn", true, [] { int v = -1; return mock.lastInt(H::SET_VEHICLE_ALARM, 1, &v) && v == 0; }, "alarm off" },
            { "stall", true, [] { int v = -1; return mock.lastInt(H::SET_VEHICLE_ENGINE_ON, 1, &v) && v == 1; }, "engine back on" },
        };

        for (auto& c : cases)
        {
            mock.set("inVehicle", c.inVehicle);
            mock.clearCalls();
            events.Clear();
            Fire(c.id);
            Check(WaitFired(c.id), std::string(c.id) + " fires");
            Check(WaitIdle(), std::string(c.id) + " finishes");
            Check(c.restored(), std::string(c.id) + ": " + c.what);
        }
        ResetState();

        Section("...including when something cuts them short");
        for (auto& c : cases)
        {
            for (const char* interrupt : { "dead", "cutscene", "loading", "switching", "online" })
            {
                ResetState();
                mock.set("inVehicle", c.inVehicle);
                mock.clearCalls();
                events.Clear();
                Fire(c.id);
                WaitFired(c.id);
                mock.set(interrupt, 1);
                PumpFor(2);
                Check(c.restored(), std::string(c.id) + " restores when interrupted by " + interrupt);
                Check(Http("GET", "/state").body.find("\"active\":[]") != std::string::npos, std::string(c.id) + " is no longer active after " + interrupt);
                ResetState();
                PumpFor(1);
            }
        }

        Section("...and when the game reloads the script");
        mock.clearCalls();
        events.Clear();
        Fire("plotarmor");
        WaitFired("plotarmor");
        mock.restart();
        PumpFor(1);
        int invincible = -1;
        Check(mock.lastInt(H::SET_PLAYER_INVINCIBLE, 1, &invincible) && invincible == 0, "a script restart can't leave the player invincible");
        events.Clear();
        Fire("fog");
        Check(WaitFired("fog"), "effects still work after a script restart");
        WaitIdle();

        Section("Timers");
        mock.clearCalls();
        events.Clear();
        Fire("moon");
        WaitFired("moon");
        mock.set("paused", 1);
        PumpFor(60);
        mock.set("paused", 0);
        mock.frame();
        Check(Http("GET", "/state").body.find("\"id\":\"moon\"") != std::string::npos, "timers freeze in the pause menu");
        WaitIdle();

        Section("Car bomb doesn't go off if cancelled");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("carbomb");
        WaitFired("carbomb");
        mock.set("dead", 1);
        PumpFor(10);
        Check(mock.count(H::EXPLODE_VEHICLE) == 0, "a cancelled bomb never explodes");
        ResetState();
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("carbomb");
        WaitFired("carbomb");
        WaitIdle();
        Check(mock.count(H::EXPLODE_VEHICLE) == 1, "an uninterrupted bomb explodes once");
        ResetState();
    }

    // ── Every effect ────────────────────────────────────────────────────────────

    std::vector<std::string> EffectIds()
    {
        std::vector<std::string> ids;
        const std::string body = Http("GET", "/state").body;
        const std::string shopKey = "\"shop\":[";
        size_t pos = body.find(shopKey);
        size_t end = body.find(']', pos);
        while ((pos = body.find("\"id\":\"", pos)) != std::string::npos && pos < end)
        {
            pos += 6;
            size_t q = body.find('"', pos);
            ids.push_back(body.substr(pos, q - pos));
            pos = q;
        }
        return ids;
    }

    void TestEveryEffect()
    {
        Section("Every effect runs and changes something");
        auto ids = EffectIds();
        printf("  (%zu effects)\n", ids.size());
        CheckEq((long long)ids.size(), 72, "the catalog is all there");

        for (auto& id : ids)
        {
            ResetState();
            const std::string settings = Http("GET", "/api/settings").body;
            size_t at = settings.find("\"id\":\"" + id + "\"");
            size_t stop = settings.find('}', at);
            const std::string entry = settings.substr(at, stop - at);
            const bool vehicle = entry.find("\"needsVehicle\":true") != std::string::npos;
            const bool silent = entry.find("\"silent\":true") != std::string::npos;

            for (int inVehicle = 0; inVehicle <= 1; ++inVehicle)
            {
                if (vehicle && !inVehicle) continue;
                if (entry.find("\"needsOnFoot\":true") != std::string::npos && inVehicle) continue;
                ClearSpawned();
                if (id == "clean") mock.set("wanted", 3);
                mock.set("inVehicle", inVehicle);
                mock.clearCalls();
                events.Clear();
                Fire(id);
                const bool fired = WaitFired(id);
                Check(fired, id + (inVehicle ? " (in vehicle) fires" : " (on foot) fires"));
                if (!fired) printf("        stream: %s\n", events.Text().c_str());
                Check(WaitIdle(), id + " finishes");
                if (silent)
                {
                    // Left off the active list on purpose, and it can take a while to land.
                    Pump([] { return mock.countActions() > 0; }, 6.0);
                    PumpFor(3);
                }
                Check(mock.countActions() > 0, id + " calls at least one game native");
                mock.set("wanted", 0);
            }
        }
        ResetState();
    }

    void TestFeedAndPlate()
    {
        Section("Feed and plate text");
        ResetState();
        Http("POST", "/api/set", "key=Alerts&value=1");
        Chat("mod_user", "!give jay 1000", "mod");
        Balance("jay");
        mock.clearCalls();
        events.Clear();
        Http("POST", "/api/simulate", "line=" + UrlEncode("@badges=;display-name=Ja~r~y :jay!jay@jay.tmi.twitch.tv PRIVMSG #test :!buy car"));
        Check(WaitFired("car"), "gift car fires");
        char text[128]{};
        mock.lastText(H::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME, text, sizeof(text));
        Check(std::string(text).find("Jary") != std::string::npos, "a viewer can't inject formatting into the feed");
        char plate[32]{};
        mock.lastText(H::SET_VEHICLE_NUMBER_PLATE_TEXT, plate, sizeof(plate));
        Check(std::string(plate) == "JARY", "the plate carries the buyer's name");
        WaitIdle();

        // Silent effects stay silent.
        mock.clearCalls();
        events.Clear();
        Fire("jumpscare");
        Check(WaitFired("jumpscare"), "jump scare fires");
        Check(mock.count(H::BEGIN_TEXT_COMMAND_THEFEED_POST) == 0, "no feed post warns the streamer");
        Check(!events.Contains("\"type\":\"sabotage\""), "no overlay toast either");
        Check(Http("GET", "/state").body.find("jumpscare\",\"name\":\"Jump Scare\",\"remaining") == std::string::npos, "and it isn't listed as active");
        Check(Pump([] { return mock.count(H::DRAW_RECT) > 0; }, 6.0), "and it does hit"); // somewhere in the next 40 game-seconds
        CheckEq(mock.count(H::ADD_EXPLOSION), 1, "with one explosion");
        int noDamage = 0;
        mock.lastInt(H::ADD_EXPLOSION, 8, &noDamage);
        Check(noDamage == 1, "the jump scare's explosion is harmless");
    }

    void TestDroppingAWeapon()
    {
        Section("Dropping a weapon");
        ResetState();
        mock.clearCalls();
        events.Clear();
        mock.set("unarmed", 1);
        Fire("butterfingers");
        PumpFor(3);
        CheckEq(mock.count(H::SET_PED_DROPS_INVENTORY_WEAPON), 0, "nothing drops with no weapon in hand");
        mock.set("unarmed", 0);
        Check(WaitFired("butterfingers"), "it fires once a weapon is in hand");
        CheckEq(mock.count(H::SET_PED_DROPS_INVENTORY_WEAPON), 1, "the gun itself drops");
        int weapon = 0, ammo = 0, nowHolding = 0;
        float forward = 0;
        mock.lastInt(H::SET_PED_DROPS_INVENTORY_WEAPON, 1, &weapon);
        Check((uint32_t)weapon == Joaat("weapon_pistol"), "the weapon that drops is the one in hand");
        mock.lastFloat(H::SET_PED_DROPS_INVENTORY_WEAPON, 3, &forward);
        Check(forward == 2.0f, "it lands 2m away");
        mock.lastInt(H::SET_PED_DROPS_INVENTORY_WEAPON, 5, &ammo);
        CheckEq(ammo, -1, "the ammo count is the -1 from the working example");
        mock.lastInt(H::SET_CURRENT_PED_WEAPON, 1, &nowHolding);
        Check((uint32_t)nowHolding == Joaat("weapon_unarmed"), "the streamer ends up empty-handed");
        WaitIdle();
    }

    void TestMoonGravity()
    {
        Section("Moon gravity");
        ResetState();
        mock.clearCalls();
        events.Clear();
        Fire("moon");
        WaitFired("moon");
        int level = -1;
        Check(mock.lastInt(H::SET_GRAVITY_LEVEL, 0, &level) && level == 1, "gravity drops");
        PumpFor(2);
        Check(mock.count(H::SET_SUPER_JUMP_THIS_FRAME) > 0, "jumps get bigger, so it shows on foot");
        WaitIdle();
        level = -1;
        Check(mock.lastInt(H::SET_GRAVITY_LEVEL, 0, &level) && level == 0, "gravity goes back to normal afterwards");
        mock.clearCalls();
        PumpFor(1);
        CheckEq(mock.count(H::SET_SUPER_JUMP_THIS_FRAME), 0, "and so do jumps");
    }

    void TestDirectCommands()
    {
        Section("Direct chat commands");
        ResetState();
        Http("POST", "/api/set", "key=ChatBonus&value=0"); // each message would otherwise earn points
        Chat("mod_user", "!give nell 100000", "mod");
        Balance("nell");
        const long long before = Balance("nell");

        mock.clearCalls();
        events.Clear();
        Chat("nell", "!feds");
        Check(WaitFired("feds"), "!feds buys the effect without !buy");
        int stars = 0;
        Check(mock.lastInt(H::SET_PLAYER_WANTED_LEVEL, 1, &stars) && stars == 5, "five stars");
        CheckEq(Balance("nell"), before - 1500, "it costs the normal price");

        mock.clearCalls();
        Chat("nell", "!fog");
        PumpFor(3);
        CheckEq(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST), 0, "an effect with no direct command ignores its bare name");
        CheckEq(Balance("nell"), before - 1500, "and charges nothing");

        const std::vector<std::string> direct = { "carmageddon", "ragdoll", "ice", "eject", "pinball", "drunk", "swap", "speed",
                                                  "honk", "madmax", "flood", "warzone", "loadout", "clones", "feds",
                                                  "aliens", "clowns", "astronauts", "zombies", "sasquatch", "lesters", "cultists",
                                                  "morph", "whoami", "abduct", "kill", "wasted" };
        Http("POST", "/api/control", "action=pause"); // so each one replies rather than firing
        for (auto& id : direct)
        {
            events.Clear();
            Chat("nell", "!" + id);
            Check(Pump([] { return events.Contains("is paused right now"); }), "!" + id + " reaches the shop");
        }
        Http("POST", "/api/control", "action=resume");
    }

    void TestChaosEffects()
    {
        Section("Carmageddon");
        ResetState();
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire("carmageddon");
        Check(WaitFired("carmageddon"), "it fires");
        const int first = mock.count(H::CREATE_VEHICLE);
        Check(first >= 10 && first <= 15, "the first 10 to 15 fall straight away (" + std::to_string(first) + ")");
        PumpFor(20);
        const int early = mock.count(H::CREATE_VEHICLE);
        // Four batches of at least 10 make 40, so fewer means they haven't all come at once.
        Check(early < 40, "the rest come later (" + std::to_string(early) + " fell in the first 20 seconds)");
        Check(WaitIdle(), "it ends within the minute");
        const int total = mock.count(H::CREATE_VEHICLE);
        Check(total >= 40 && total <= 60, "four batches of 10 to 15 (" + std::to_string(total) + " in all)");
        CheckEq(mock.count(H::SET_ENTITY_ROTATION), total, "each one tumbling");
        CheckEq(mock.count(H::SET_ENTITY_AS_NO_LONGER_NEEDED), total, "each one handed straight back to the game");
        Check(mock.count(H::SET_MODEL_AS_NO_LONGER_NEEDED) >= 9, "and the models are let go at the end");
        float fall = 0;
        Check(mock.lastFloat(H::SET_ENTITY_VELOCITY, 3, &fall) && fall <= -25.0f && fall >= -40.0f, "falling at 25 to 40 m/s");
        // The player is at (10, 20). Within 35 degrees of straight ahead, the distance across is
        // at most 0.71 of the distance forward.
        const auto allAhead = [](float forwardX, float forwardY)
        {
            int seen = 0;
            float x = 0, y = 0;
            for (; mock.nthFloat(H::CREATE_VEHICLE, seen, 1, &x) && mock.nthFloat(H::CREATE_VEHICLE, seen, 2, &y); ++seen)
            {
                const float dx = x - 10.0f, dy = y - 20.0f;
                const float along = dx * forwardX + dy * forwardY, across = std::abs(dx * forwardY - dy * forwardX);
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (along <= 0.0f || across > along * 0.71f || distance < 14.9f || distance > 60.1f) return false;
            }
            return seen >= 40;
        };
        Check(allAhead(0.0f, 1.0f), "all in front of the camera, 15 to 60m away");
        mock.set("camYaw", 90);
        mock.clearCalls();
        events.Clear();
        Fire("carmageddon");
        Check(WaitFired("carmageddon") && WaitIdle() && allAhead(-1.0f, 0.0f), "wherever the camera faces");
        mock.set("camYaw", 0);
        events.Clear();
        mock.set("spawnFails", 1);
        Fire("carmageddon");
        Check(Pump([] { return events.Contains("\"type\":\"failed\""); }), "it reports failure when nothing spawns");
        ResetState();

        Section("Ragdoll");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("ragdoll");
        Check(WaitFired("ragdoll"), "it fires in a vehicle");
        int flag = 0;
        Check(mock.lastInt(H::TASK_LEAVE_VEHICLE, 2, &flag) && flag == 4160, "the streamer bails out");
        PumpFor(1);
        CheckEq(mock.count(H::SET_PED_TO_RAGDOLL), 0, "no ragdoll until they're out");
        mock.set("inVehicle", 0);
        Check(Pump([] { return mock.count(H::SET_PED_TO_RAGDOLL) > 0; }, 3.0), "then they go down");
        int shortest = -1, longest = -2;
        mock.lastInt(H::SET_PED_TO_RAGDOLL, 1, &shortest);
        mock.lastInt(H::SET_PED_TO_RAGDOLL, 2, &longest);
        Check(shortest == longest && shortest > 0, "the time down is fixed, so they can't get up early");
        WaitIdle();
        mock.clearCalls();
        events.Clear();
        mock.set("ragdolling", 1);
        Fire("ragdoll");
        WaitFired("ragdoll");
        PumpFor(1);
        CheckEq(mock.count(H::SET_PED_TO_RAGDOLL), 0, "someone already down isn't knocked down again");
        CheckEq(mock.count(H::TASK_LEAVE_VEHICLE), 0, "and nobody on foot tries to bail out");
        WaitIdle();
        ResetState();

        Section("Ice tires");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("ice");
        WaitFired("ice");
        int grip = -1;
        Check(Pump([] { return mock.count(H::SET_VEHICLE_REDUCE_GRIP) > 0; }, 3.0) && mock.lastInt(H::SET_VEHICLE_REDUCE_GRIP, 1, &grip) && grip == 1, "grip drops");
        mock.set("inVehicle", 0);
        Check(WaitIdle(), "it ends when the streamer gets out");
        Check(mock.lastInt(H::SET_VEHICLE_REDUCE_GRIP, 1, &grip) && grip == 0, "and grip comes back");
        ResetState();

        Section("Ejector seat");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("eject");
        WaitFired("eject");
        float vx = 0, vy = 0, vz = 0;
        mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &vx);
        mock.lastFloat(H::SET_ENTITY_VELOCITY, 2, &vy);
        mock.lastFloat(H::SET_ENTITY_VELOCITY, 3, &vz);
        Check(vx == 4.0f && vy == 3.0f && vz == 25.0f, "straight up, with half the car's momentum");
        CheckEq(mock.count(H::SET_VEHICLE_DOOR_OPEN), 1, "the door opens");
        CheckEq(mock.count(H::LOWER_CONVERTIBLE_ROOF), 0, "and the roof is left alone on a hardtop");
        int given = 0;
        Check(mock.lastInt(H::GIVE_WEAPON_TO_PED, 1, &given) && (uint32_t)given == Joaat("gadget_parachute"), "it gives a parachute by default");
        WaitIdle();
        mock.set("convertible", 1);
        Http("POST", "/api/set", "key=EjectParachute&value=0");
        mock.clearCalls();
        events.Clear();
        Fire("eject");
        WaitFired("eject");
        CheckEq(mock.count(H::LOWER_CONVERTIBLE_ROOF), 1, "a convertible's roof comes down");
        CheckEq(mock.count(H::SET_VEHICLE_DOOR_OPEN), 0, "instead of the door");
        CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 0, "and the setting can withhold the parachute");
        Http("POST", "/api/set", "key=EjectParachute&value=1");
        WaitIdle();
        ResetState();

        Section("Pinball");
        mock.set("collided", 1);
        mock.set("floorHit", 1);
        mock.clearCalls();
        events.Clear();
        Fire("pinball");
        WaitFired("pinball");
        PumpFor(3);
        CheckEq(mock.count(H::SET_ENTITY_VELOCITY), 0, "standing on the floor doesn't bounce");
        mock.set("floorHit", 0);
        Check(Pump([] { return mock.count(H::SET_ENTITY_VELOCITY) > 0; }, 3.0), "hitting a wall does");
        float bounce = 0;
        Check(mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -20.0f, "away from the wall, and clamped");
        Check(mock.count(H::SET_PED_TO_RAGDOLL) > 0, "someone on foot goes flying");
        int recording = 0;
        Check(mock.lastInt(H::SET_ENTITY_RECORDS_COLLISIONS, 0, &recording) && recording == 100, "the game is told to record their hits");
        mock.clearCalls();
        PumpFor(1);
        CheckEq(mock.count(H::SET_ENTITY_VELOCITY), 0, "and isn't thrown again until they've had time to get up");
        mock.set("ragdolling", 1);
        const auto thrown = [] { return Pump([] { return mock.count(H::SET_ENTITY_VELOCITY) > 0; }, 3.0); };
        Check(thrown(), "only the wait holds it back, not the game's ragdoll check");
        mock.set("collided", 0);
        WaitIdle();

        // Each of these starts a fresh one, so the wait between throws can't get in the way.
        const auto firePinball = []
        {
            mock.clearCalls();
            events.Clear();
            Fire("pinball");
            WaitFired("pinball");
        };
        ResetState();
        mock.set("stopped", 1);
        firePinball();
        PumpFor(1);
        mock.set("collided", 1);
        Check(thrown() && mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -12.0f, "barely touching a wall still bounces");
        mock.set("collided", 0);
        WaitIdle();

        ResetState();
        firePinball();
        PumpFor(1);
        mock.set("collided", 1);
        mock.set("stopped", 1);
        Check(thrown() && mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -20.0f, "it goes by the speed going in, not what the hit left");
        mock.set("collided", 0);
        WaitIdle();

        ResetState();
        mock.set("wallAhead", 1);
        firePinball();
        Check(thrown() && mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -20.0f, "walking into a wall the game didn't report throws them away from it");
        Check(mock.count(H::SET_PED_TO_RAGDOLL) > 0, "and off their feet");
        mock.set("wallAhead", 0);
        WaitIdle();

        ResetState();
        mock.set("stopped", 1);
        mock.set("wallAhead", 1);
        firePinball();
        PumpFor(1);
        CheckEq(mock.count(H::SET_ENTITY_VELOCITY), 0, "standing still beside it doesn't");
        mock.set("held32", 1);
        Check(thrown() && mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -12.0f, "pushing against it does");
        mock.set("wallAhead", 0);
        WaitIdle();

        ResetState();
        mock.set("inVehicle", 1);
        mock.set("collided", 1);
        firePinball();
        Check(thrown(), "a vehicle bounces too");
        int bounced = 0;
        Check(mock.lastInt(H::SET_ENTITY_VELOCITY, 0, &bounced) && bounced == 200, "the vehicle itself");
        Check(mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &bounce) && bounce == -40.0f, "harder than on foot");
        CheckEq(mock.count(H::SET_PED_TO_RAGDOLL), 0, "with nobody knocked out of it");
        CheckEq(mock.count(H::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE), 0, "a vehicle only goes by what the game reports");
        CheckEq(mock.count(H::SET_ENTITY_RECORDS_COLLISIONS), 0, "and its recording is left alone");
        mock.set("ceilingHit", 1);
        mock.clearCalls();
        Check(Pump([] { return mock.count(H::SET_ENTITY_VELOCITY) > 0; }, 3.0) && mock.lastFloat(H::SET_ENTITY_VELOCITY, 3, &bounce) && bounce == 3.0f, "a ceiling can't drive it into the ground");
        mock.set("collided", 0);
        WaitIdle();
        ResetState();

        Section("Backwards controls");
        constexpr int kMoveLR = 30, kMoveUD = 31, kVehLR = 59;
        mock.set("held35", 1);
        mock.set("held32", 1);
        mock.clearCalls();
        events.Clear();
        Fire("drunk");
        WaitFired("drunk");
        mock.clearCalls();
        Check(Pump([] { return mock.count(H::SET_CONTROL_VALUE_NEXT_FRAME) > 0; }, 3.0), "it feeds the controls back in");
        CheckEq(mock.count(H::DISABLE_CONTROL_ACTION), 0, "without blocking them, or the player would freeze");
        Check(mock.fedControl(kMoveLR) == -1.0f, "holding right walks left");
        Check(mock.fedControl(kMoveUD) == 1.0f, "holding forward walks backward");
        mock.set("held35", 0);
        mock.set("held32", 0);
        mock.set("held34", 1);
        mock.set("held33", 1);
        mock.clearCalls();
        Pump([] { return mock.count(H::SET_CONTROL_VALUE_NEXT_FRAME) > 0; }, 3.0);
        Check(mock.fedControl(kMoveLR) == 1.0f, "holding left walks right");
        Check(mock.fedControl(kMoveUD) == -1.0f, "holding back walks forward");
        mock.set("held34", 0);
        mock.set("held33", 0);
        mock.clearCalls();
        Pump([] { return mock.count(H::SET_CONTROL_VALUE_NEXT_FRAME) > 0; }, 3.0);
        Check(mock.fedControl(kMoveLR) == 0.0f && mock.fedControl(kMoveUD) == 0.0f, "holding nothing stays put");
        mock.set("inVehicle", 1);
        mock.set("held64", 1);
        mock.clearCalls();
        Pump([] { return mock.count(H::SET_CONTROL_VALUE_NEXT_FRAME) > 0; }, 3.0);
        Check(mock.fedControl(kVehLR) == -1.0f, "steering right turns left");
        mock.set("held64", 0);
        mock.set("held63", 1);
        mock.clearCalls();
        Pump([] { return mock.count(H::SET_CONTROL_VALUE_NEXT_FRAME) > 0; }, 3.0);
        Check(mock.fedControl(kVehLR) == 1.0f, "steering left turns right");
        WaitIdle();
        ResetState();

        Section("Vehicle roulette");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("swap");
        Check(WaitFired("swap"), "it fires");
        CheckEq(mock.count(H::DELETE_VEHICLE), 1, "the old vehicle is deleted");
        CheckEq(mock.count(H::CREATE_VEHICLE), 1, "a new one takes its place");
        CheckEq(mock.count(H::SET_PED_INTO_VEHICLE), 1, "with the streamer driving");
        float carried = 0;
        Check(mock.lastFloat(H::SET_ENTITY_VELOCITY, 1, &carried) && carried == 8.0f, "and its momentum carries over");
        WaitIdle();
        ResetState();

        Section("Lock throttle");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("speed");
        WaitFired("speed");
        float held = 0;
        Check(Pump([] { return mock.count(H::SET_VEHICLE_FORWARD_SPEED) > 0; }, 3.0) && mock.lastFloat(H::SET_VEHICLE_FORWARD_SPEED, 1, &held) && held == 40.0f, "it holds top speed");
        Check(mock.count(H::DISABLE_CONTROL_ACTION) >= 2, "with no brakes");
        mock.set("inVehicle", 0);
        Check(WaitIdle(), "it ends when the streamer gets out");
        ResetState();

        Section("Exploding horn");
        mock.set("inVehicle", 1);
        mock.clearCalls();
        events.Clear();
        Fire("honk");
        WaitFired("honk");
        int proof = 0;
        Check(mock.lastInt(H::SET_ENTITY_PROOFS, 3, &proof) && proof == 1, "the car and driver are explosion-proof");
        PumpFor(3);
        CheckEq(mock.count(H::ADD_EXPLOSION), 0, "nothing explodes without the horn");
        mock.set("hornHeld", 1);
        Check(Pump([] { return mock.count(H::ADD_EXPLOSION) > 0; }, 3.0), "the horn sets one off");
        PumpFor(3);
        Check(mock.count(H::ADD_EXPLOSION) <= 7, "at most one every half second");
        mock.set("hornHeld", 0);
        WaitIdle();
        ResetState();

        Section("Mad Max traffic");
        mock.set("noDrivers", 1);
        mock.clearCalls();
        events.Clear();
        Fire("madmax");
        PumpFor(3);
        CheckEq(mock.count(H::GET_PED_IN_VEHICLE_SEAT), 0, "empty cars don't get given drivers");
        CheckEq(mock.count(H::TASK_VEHICLE_MISSION_PED_TARGET), 0, "and it waits for traffic");
        mock.set("noDrivers", 0);
        Check(WaitFired("madmax"), "it fires when there's traffic");
        CheckEq(mock.count(H::TASK_VEHICLE_MISSION_PED_TARGET), 2, "every driver goes for the streamer");
        int mission = 0, target = 0;
        mock.lastInt(H::TASK_VEHICLE_MISSION_PED_TARGET, 3, &mission);
        mock.lastInt(H::TASK_VEHICLE_MISSION_PED_TARGET, 2, &target);
        Check(mission == 2 && target == 100, "to ram them");
        WaitIdle();
        CheckEq(mock.count(H::TASK_VEHICLE_DRIVE_WANDER), 2, "then they go back to driving around");
        int group = 0;
        Check(mock.lastInt(H::SET_PED_RELATIONSHIP_GROUP_HASH, 1, &group) && group == 0x1234, "in their old group");
        ResetState();

        Section("Warzone rounds");
        mock.clearCalls();
        events.Clear();
        Fire("warzone");
        WaitFired("warzone");
        Check(Pump([] { return mock.count(H::SET_EXPLOSIVE_AMMO_THIS_FRAME) > 0; }, 3.0), "the streamer gets explosive ammo");
        CheckEq(mock.count(H::ADD_EXPLOSION), 0, "nothing explodes while nobody's shooting");
        mock.set("pedsShooting", 1);
        Check(Pump([] { return mock.count(H::ADD_EXPLOSION) > 0; }, 3.0), "a ped's shot explodes where it lands");
        PumpFor(3);
        const int firstHits = mock.count(H::ADD_EXPLOSION);
        Check(firstHits <= 2, "the same impact doesn't keep exploding");
        mock.set("impactZ", 5);
        Check(Pump([&] { return mock.count(H::ADD_EXPLOSION) > firstHits; }, 3.0), "a new impact does");
        mock.set("pedsShooting", 0);
        WaitIdle();
        mock.clearCalls();
        PumpFor(2);
        CheckEq(mock.count(H::SET_EXPLOSIVE_AMMO_THIS_FRAME), 0, "explosive ammo stops with the effect");
        ResetState();

        Section("Weapon roulette");
        mock.clearCalls();
        events.Clear();
        Fire("loadout");
        WaitFired("loadout");
        CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 1, "a weapon the streamer doesn't own gets lent");
        int canSwitch = -1;
        Check(mock.lastInt(H::SET_PED_CAN_SWITCH_WEAPON, 1, &canSwitch) && canSwitch == 0, "and they can't switch away");
        Check(Pump([&] { return mock.lastInt(H::SET_PED_CAN_SWITCH_WEAPON, 1, &canSwitch) && canSwitch == 1; }, 3.0), "for a few seconds");
        WaitIdle();
        CheckEq(mock.count(H::REMOVE_WEAPON_FROM_PED), 1, "then the lent weapon goes back");
        mock.set("ownsWeapon", 1);
        mock.clearCalls();
        events.Clear();
        Fire("loadout");
        WaitFired("loadout");
        CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 0, "a weapon they own isn't given again");
        WaitIdle();
        CheckEq(mock.count(H::REMOVE_WEAPON_FROM_PED), 0, "and it isn't taken away");
        ResetState();

        Section("Clone swarm");
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire("clones");
        Check(WaitFired("clones"), "it fires");
        CheckEq(mock.count(H::CREATE_PED), 10, "ten clones");
        CheckEq(mock.count(H::TASK_COMBAT_PED), 10, "all attacking");
        CheckEq(mock.count(H::SET_ENTITY_AS_NO_LONGER_NEEDED), 10, "all handed straight back to the game");
        WaitIdle();
        ResetState();

        Section("Five-star sting");
        Http("POST", "/api/set", "key=FedsClearSeconds&value=3");
        mock.clearCalls();
        events.Clear();
        Fire("feds");
        WaitFired("feds");
        CheckEq(mock.count(H::SET_PLAYER_WANTED_LEVEL_NOW), 1, "the stars land right away");
        Check(mock.count(H::ENABLE_DISPATCH_SERVICE) > 0, "with the cops dispatched");
        WaitIdle();
        CheckEq(mock.count(H::CLEAR_PLAYER_WANTED_LEVEL), 1, "and the setting can clear them after a while");
        Http("POST", "/api/set", "key=FedsClearSeconds&value=0");
        mock.set("wanted", 5);
        events.Clear();
        Fire("feds");
        Check(Pump([] { return events.Contains("the cops are already on it"); }), "it's refused at five stars already");
        ResetState();

        Section("Flash flood");
        mock.set("inVehicle", 1);
        mock.set("inWater", 1);
        mock.clearCalls();
        events.Clear();
        Fire("flood");
        WaitFired("flood");
        Check(Pump([] { return mock.count(H::MODIFY_WATER) > 0; }, 3.0), "the water moves");
        int engine = -1;
        Check(Pump([&] { return mock.lastInt(H::SET_VEHICLE_ENGINE_ON, 1, &engine) && engine == 0; }, 3.0), "a car in the water stalls");
        WaitIdle();
        Check(mock.lastInt(H::SET_VEHICLE_ENGINE_ON, 1, &engine) && engine == 1, "and starts again afterwards");
        ResetState();
    }

    // Fires one effect from a clean slate, with some of the mock's state set first, and waits for it.
    bool FireFreshWith(const std::string& id, std::initializer_list<std::pair<const char*, int>> state)
    {
        ResetState();
        for (auto& [key, value] : state) mock.set(key, value);
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire(id);
        return WaitFired(id);
    }

    bool FireFresh(const std::string& id) { return FireFreshWith(id, {}); }

    void TestSwarms()
    {
        Section("Character swarms");
        struct Case { const char* id; const char* model; const char* weapon; };
        const Case cases[] = {
            { "aliens", "s_m_m_movalien_01", "weapon_stungun" }, { "clowns", "s_m_y_clown_01", "weapon_bat" },
            { "astronauts", "s_m_m_movspace_01", "weapon_raypistol" }, { "zombies", "u_m_y_zombie_01", nullptr },
            { "sasquatch", "cs_orleans", nullptr }, { "lesters", "ig_lestercrest", "weapon_golfclub" },
            { "cultists", "a_m_y_acult_01", "weapon_machete" },
        };
        for (const Case& sc : cases)
        {
            const std::string id = sc.id;
            Check(FireFresh(id), id + " fires");
            CheckEq(mock.count(H::CREATE_PED), 8, id + ": eight of them");
            int value = 0;
            Check(mock.lastInt(H::CREATE_PED, 1, &value) && (uint32_t)value == Joaat(sc.model), id + ": the right model");
            if (sc.weapon)
            {
                CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 8, id + ": all armed");
                Check(mock.lastInt(H::GIVE_WEAPON_TO_PED, 1, &value) && (uint32_t)value == Joaat(sc.weapon), id + ": with the right weapon");
            }
            else CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 0, id + ": bare-handed");
            CheckEq(mock.count(H::TASK_COMBAT_PED), 8, id + ": all attacking");
            CheckEq(mock.count(H::SET_ENTITY_AS_NO_LONGER_NEEDED), 8, id + ": all handed straight back to the game");
            CheckEq(mock.count(H::ADD_BLIP_FOR_ENTITY), 8, id + ": each with a blip");
            CheckEq(mock.count(H::SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER), id == "sasquatch" ? 1 : 0, id + ": only sasquatches change melee damage");
        }

        // Where they stand and how they fight, on one swarm. The mock has no safe spots for peds,
        // so each lands exactly on the ring it was aimed at, around the player at (10, 20).
        Check(FireFresh("clowns"), "clowns fire again");
        std::set<int> sides;
        bool inRange = true, facingIn = true;
        for (int n = 0; n < 8; ++n)
        {
            float x = 0, y = 0, heading = 0;
            if (!mock.nthFloat(H::CREATE_PED, n, 2, &x) || !mock.nthFloat(H::CREATE_PED, n, 3, &y) || !mock.nthFloat(H::CREATE_PED, n, 5, &heading))
            {
                inRange = facingIn = false;
                break;
            }
            const float dx = x - 10.0f, dy = y - 20.0f, distance = std::sqrt(dx * dx + dy * dy);
            if (distance < 11.9f || distance > 25.1f) inRange = false;
            sides.insert((dx >= 0 ? 1 : 0) + (dy >= 0 ? 2 : 0));
            // A heading faces (-sin, cos), anticlockwise from north. That should point back at the player.
            const double h = heading * 3.14159265358979 / 180.0;
            if ((-std::sin(h) * -dx + std::cos(h) * -dy) / distance < 0.999) facingIn = false;
        }
        Check(inRange, "they spawn 12 to 25m out");
        CheckEq((long long)sides.size(), 4, "on every side");
        Check(facingIn, "facing the streamer");
        int value = -1;
        Check(mock.lastInt(H::SET_PED_COMBAT_ABILITY, 1, &value) && value == 2, "professional fighters");
        Check(mock.lastInt(H::SET_PED_COMBAT_MOVEMENT, 1, &value) && value == 2, "who always advance");
        Check(mock.lastInt(H::SET_PED_COMBAT_RANGE, 1, &value) && value == 0, "to close range");
        Check(mock.lastInt(H::SET_BLIP_COLOUR, 1, &value) && value == 1 && mock.lastInt(H::SET_BLIP_AS_FRIENDLY, 1, &value) && value == 0,
              "red enemy blips");
        PumpFor(5);
        CheckEq(mock.count(H::REMOVE_BLIP), 0, "the blips stay while they're alive");
        mock.set("othersDead", 1);
        Check(Pump([] { return mock.count(H::REMOVE_BLIP) == 8; }, 3.0), "and come off when they die");
        mock.set("othersDead", 0);
        PumpFor(2);
        CheckEq(mock.count(H::REMOVE_BLIP), 8, "once each");

        Check(FireFresh("lesters"), "lesters fire");
        mock.clearCalls();
        ClearSpawned();
        CheckEq(mock.count(H::REMOVE_BLIP), 8, "a blip comes off when the game clears its attacker away");

        Check(FireFresh("sasquatch"), "sasquatches fire");
        float boost = 0;
        Check(mock.lastFloat(H::SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER, 0, &boost) && boost == 3.0f, "sasquatch punches hit three times as hard");
        PumpFor(5);
        CheckEq(mock.count(H::RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER), 0, "for as long as any are standing");
        mock.set("othersDead", 1);
        Check(Pump([] { return mock.count(H::RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER) == 1; }, 3.0), "and back to normal once they're all down");
        mock.set("othersDead", 0);

        Check(FireFresh("aliens"), "aliens fire");
        PumpFor(30);
        CheckEq(mock.count(H::TASK_SMART_FLEE_PED), 0, "aliens fight for a while");
        Check(Pump([] { return mock.count(H::TASK_SMART_FLEE_PED) == 8; }, 3.0), "then any still standing run off after 45 seconds");
        CheckEq(mock.count(H::REMOVE_BLIP), 8, "and lose their blips");

        Check(FireFresh("aliens"), "aliens fire again");
        mock.set("paused", 1);
        PumpFor(60);
        mock.set("paused", 0);
        PumpFor(20);
        CheckEq(mock.count(H::TASK_SMART_FLEE_PED), 0, "time in the pause menu doesn't count toward that");
        Check(Pump([] { return mock.count(H::TASK_SMART_FLEE_PED) == 8; }, 3.0), "but play time does");

        // The game hasn't been seen to lack any of these, so this only proves the fallbacks work.
        ResetState();
        mock.set("missingModel", (int)Joaat("cs_orleans"));
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire("sasquatch");
        Check(WaitFired("sasquatch"), "a swarm whose first model is missing still fires");
        Check(mock.lastInt(H::CREATE_PED, 1, &value) && (uint32_t)value == Joaat("ig_orleans"), "with the next model");
        ResetState();
        mock.set("missingWeapon", (int)Joaat("weapon_raypistol"));
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire("astronauts");
        Check(WaitFired("astronauts"), "and one whose first weapon is missing");
        Check(mock.lastInt(H::GIVE_WEAPON_TO_PED, 1, &value) && (uint32_t)value == Joaat("weapon_pistol"), "with the next weapon");

        // They're handed back to the game straight away, but they still count against the cap of 40.
        ResetState();
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        for (const char* id : { "clowns", "astronauts", "zombies", "lesters", "cultists" }) Fire(id);
        Check(Pump([] { return mock.count(H::CREATE_PED) == 40; }, 6.0), "five swarms make 40 attackers");
        Fire("sasquatch");
        PumpFor(8);
        CheckEq(mock.count(H::CREATE_PED), 40, "a sixth waits while all 40 are standing");
        ClearSpawned();
        Check(WaitFired("sasquatch"), "and comes once there's room");
        Http("POST", "/api/control", "action=clear");
        ClearSpawned();
        ResetState();
    }

    // The streamer starts as ped 100, so they're back in their own body once control goes back to it.
    bool BackInOwnBody() { return Pump([] { return mock.countArg(H::CHANGE_PLAYER_PED, 1, 100) > 0; }, 3.0); }

    void TestMorph()
    {
        Section("Morph");
        const int mime = (int)Joaat("s_m_y_mime"), chimp = (int)Joaat("a_c_chimp");
        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        int value = 0;
        Check(mock.lastInt(H::CHANGE_PLAYER_PED, 1, &value) && value >= 1000, "the streamer takes over a new body");
        Check(mock.lastInt(H::CREATE_PED, 1, &value) && value == mime, "the only one this game has, so the first one listed isn't required");
        Check(mock.lastInt(H::GIVE_WEAPON_TO_PED, 1, &value) && (uint32_t)value == Joaat("weapon_pistol") &&
              mock.lastInt(H::GIVE_WEAPON_TO_PED, 2, &value) && value == 120, "holding a copy of the streamer's pistol, with the same ammo");
        Check(mock.lastInt(H::SET_ENTITY_HEALTH, 1, &value) && value == 150 + 5000 && mock.lastInt(H::SET_PED_MAX_HEALTH, 1, &value) && value == 200 + 5000 &&
              mock.lastInt(H::SET_PED_ARMOUR, 1, &value) && value == 50,
              "and the same health and armor, with 5,000 health to spare so the body never really dies");
        Check(mock.countArg(H::SET_ENTITY_VISIBLE, 0, 100) == 1 && mock.lastInt(H::SET_ENTITY_VISIBLE, 1, &value) && value == 0 &&
              mock.lastInt(H::FREEZE_ENTITY_POSITION, 1, &value) && value == 1 && mock.lastInt(H::SET_ENTITY_INVINCIBLE, 1, &value) && value == 1,
              "their own ped waits, hidden, frozen and untouchable");
        CheckEq(mock.countArg(H::SET_ENTITY_AS_MISSION_ENTITY, 0, 100), 1, "where the game won't clear it away");
        PumpFor(2);
        Check(mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 23) > 0 && mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 27) > 0 &&
              mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 19) > 0, "no getting into vehicles, no phone and no switching characters");
        Check(WaitIdle(), "it ends");
        Check(BackInOwnBody(), "then the streamer is back in their own body");
        CheckEq(mock.count(H::DELETE_PED), 1, "the borrowed one is deleted");
        Check(mock.lastInt(H::SET_ENTITY_VISIBLE, 1, &value) && value == 1 && mock.lastInt(H::FREEZE_ENTITY_POSITION, 1, &value) && value == 0 &&
              mock.lastInt(H::SET_ENTITY_INVINCIBLE, 1, &value) && value == 0, "and theirs is visible, free and mortal again");
        CheckEq(mock.count(H::SET_PLAYER_MODEL), 0, "no model changes, so nothing they carry or wear is lost");

        Section("...as an animal");
        Check(FireFreshWith("morph", { { "onlyModel", chimp } }), "a chimp fires");
        Check(mock.lastInt(H::CREATE_PED, 0, &value) && value == 28, "it's an animal");
        CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 0, "with no weapon");
        PumpFor(1);
        Check(mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 37) > 0, "and no weapon wheel");
        mock.clearCalls();
        events.Clear();
        Fire("guns");
        PumpFor(5);
        CheckEq(mock.count(H::GIVE_WEAPON_TO_PED), 0, "Arms Dealer waits while the streamer is an animal");
        Check(WaitFired("guns"), "and comes once they're themselves again");
        WaitIdle();

        Section("...with attackers around");
        FireFresh("zombies"); // so the mod's hostile group exists, then the world's peds join it
        mock.set("pedsHostile", 1);
        mock.set("onlyModel", mime);
        mock.clearCalls();
        events.Clear();
        Fire("morph");
        Check(WaitFired("morph"), "it fires");
        int body = 0;
        mock.lastInt(H::CHANGE_PLAYER_PED, 1, &body);
        CheckEq(mock.countArg(H::TASK_COMBAT_PED, 1, body), 2, "attackers on foot go after the new body");
        Check(WaitIdle() && BackInOwnBody(), "it ends");
        CheckEq(mock.countArg(H::TASK_COMBAT_PED, 1, 100), 2, "then after the streamer's own again");

        // Story mode can't respawn the player as anyone but its three characters, so the streamer
        // must never die in the body.
        Section("...when the body's health runs out");
        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("othersHurt", 40);
        PumpFor(2);
        CheckEq(mock.countArg(H::CHANGE_PLAYER_PED, 1, 100), 0, "losing some of the streamer's share of it changes nothing");
        mock.set("othersHurt", 60);
        Check(BackInOwnBody(), "once their share is gone, they're back in their own body straight away");
        Check(Http("GET", "/state").body.find("\"active\":[]") != std::string::npos, "the morph is over");
        Check(mock.lastInt(H::SET_ENTITY_HEALTH, 0, &value) && value == 100 && mock.lastInt(H::SET_ENTITY_HEALTH, 1, &value) && value == 0,
              "and they die as themselves, which the game can respawn");
        mock.set("othersHurt", 0);

        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("dead", 1);
        Check(BackInOwnBody(), "if the body dies outright anyway, they're put back in their own body without waiting");
        Check(mock.lastInt(H::SET_ENTITY_HEALTH, 0, &value) && value == 100 && mock.lastInt(H::SET_ENTITY_HEALTH, 1, &value) && value == 0,
              "and die as themselves");
        mock.set("dead", 0);
        PumpFor(1);

        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("arrested", 1);
        Check(BackInOwnBody(), "being busted puts them back in their own body without waiting too");
        CheckEq(mock.countArg(H::SET_ENTITY_HEALTH, 0, 100), 0, "without killing them");
        mock.set("arrested", 0);
        PumpFor(1);

        Section("...when it can't change back straight away");
        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("inAir", 1);
        PumpFor(35);
        CheckEq(mock.countArg(H::CHANGE_PLAYER_PED, 1, 100), 0, "in mid-air they stay in the body");
        mock.set("inAir", 0);
        Check(BackInOwnBody(), "until they land");

        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("entitiesGone", 1);
        Check(Pump([] { int m = 0; return mock.lastInt(H::SET_PLAYER_MODEL, 1, &m) && (uint32_t)m == Joaat("player_one"); }, 3.0),
              "if the game took their own ped away, at least their character's model comes back");
        mock.set("entitiesGone", 0);

        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "it fires");
        mock.set("playerPed", 300); // a save loaded, and the game has given the player another ped
        // Time is capped per frame, so a slow frame stretches the 30 seconds. Wait for the end itself.
        Check(Pump([] { return mock.count(H::DELETE_PED) > 0; }, 3.0), "it still ends");
        CheckEq(mock.count(H::CHANGE_PLAYER_PED), 1, "after a save loads, the new player is left alone");
        Check(mock.lastInt(H::SET_ENTITY_VISIBLE, 0, &value) && value == 100 && mock.lastInt(H::SET_ENTITY_VISIBLE, 1, &value) && value == 1,
              "the old ped goes back to the game as it was");
        CheckEq(mock.count(H::DELETE_PED), 1, "and the borrowed body is deleted");
        ResetState();
        PumpFor(1);
    }

    void TestWasted()
    {
        Section("Wasted");
        int value = 0;
        auto diedAsThemselves = [&] { return mock.lastInt(H::SET_ENTITY_HEALTH, 0, &value) && value == 100 && mock.lastInt(H::SET_ENTITY_HEALTH, 1, &value) && value == 0; };
        Check(FireFresh("kill"), "it fires");
        Check(diedAsThemselves(), "the streamer drops dead");

        Check(FireFresh("plotarmor"), "Plot Armor fires");
        mock.clearCalls();
        events.Clear();
        Fire("kill");
        PumpFor(10);
        CheckEq(mock.count(H::SET_ENTITY_HEALTH), 0, "Wasted waits while the streamer is invincible");
        Check(WaitFired("kill"), "and comes once the armor wears off");
        Check(diedAsThemselves(), "then the streamer drops dead");
        WaitIdle();

        // Story mode can't respawn the player as anyone but its three characters, so a morph ends first.
        Section("...while morphed");
        const int mime = (int)Joaat("s_m_y_mime");
        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "a morph fires");
        int body = 0;
        mock.lastInt(H::CHANGE_PLAYER_PED, 1, &body);
        mock.clearCalls();
        events.Clear();
        Fire("kill");
        Check(WaitFired("kill"), "Wasted fires");
        // 0.3 seconds is about 9 seconds in the game, well short of the morph's 30.
        Check(Pump([] { return mock.countArg(H::CHANGE_PLAYER_PED, 1, 100) > 0; }, 0.3), "the streamer is put back in their own body straight away");
        Check(Http("GET", "/state").body.find("\"active\":[]") != std::string::npos, "the morph is over");
        Check(diedAsThemselves(), "and they die as themselves, which the game can respawn");
        CheckEq(mock.countArg(H::SET_ENTITY_HEALTH, 0, body), 0, "never in the body");
        CheckEq(mock.count(H::DELETE_PED), 1, "which is deleted");

        Check(FireFreshWith("morph", { { "onlyModel", mime } }), "a morph fires");
        mock.set("inAir", 1);
        Check(WaitIdle(), "it ends");
        CheckEq(mock.countArg(H::CHANGE_PLAYER_PED, 1, 100), 0, "but in mid-air the streamer is still in the body");
        events.Clear();
        Fire("kill");
        Check(WaitFired("kill"), "Wasted fires");
        Check(BackInOwnBody(), "they're put back without waiting to land");
        Check(diedAsThemselves(), "and die as themselves");
        ResetState();
        PumpFor(1);
    }

    void TestAbduction()
    {
        Section("Alien abduction");
        Check(FireFresh("abduct"), "it fires");
        CheckEq(mock.countArg(H::GIVE_WEAPON_TO_PED, 1, (int)Joaat("gadget_parachute")), 1, "with a parachute on the streamer's back");
        PumpFor(1);
        float lift = 0;
        Check(mock.lastFloat(H::SET_ENTITY_VELOCITY, 3, &lift) && lift == 3.5f, "a beam lifts them at 3.5 m/s");
        int value = -1;
        Check(mock.lastInt(H::DRAW_SHADOWED_SPOT_LIGHT, 6, &value) && value == 0 && mock.lastInt(H::DRAW_SHADOWED_SPOT_LIGHT, 7, &value) && value == 255,
              "under a turquoise spotlight");
        Check(mock.count(H::DRAW_MARKER) > 0, "with the beam itself showing");
        Check(mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 30) > 0 && mock.countArg(H::DISABLE_CONTROL_ACTION, 1, 22) > 0, "and they can't walk or jump out of it");
        Pump([] { return mock.count(H::CREATE_PED) > 0; }, 3.0); // they all arrive in the same frame
        const int aliens = mock.count(H::CREATE_PED);
        Check(aliens >= 3 && aliens <= 4, "3 or 4 aliens appear below (" + std::to_string(aliens) + ")");
        CheckEq(mock.count(H::TASK_AIM_GUN_AT_COORD), aliens, "all aiming up");
        float aim = 0;
        Check(mock.lastFloat(H::TASK_AIM_GUN_AT_COORD, 3, &aim) && aim == 55.0f, "at where the beam lets go, 25m up");
        mock.set("inAir", 1);
        mock.set("raisedBy", 26);
        Check(Pump([] { return mock.count(H::FORCE_PED_TO_OPEN_PARACHUTE) > 0; }, 3.0), "at the top the parachute opens");
        CheckEq(mock.count(H::DELETE_PED), aliens, "and the aliens vanish");
        Check(mock.lastInt(H::ADD_EXPLOSION, 6, &value) && value == 1 && mock.lastInt(H::ADD_EXPLOSION, 8, &value) && value == 1, "with a harmless shockwave");
        CheckEq(mock.count(H::START_PARTICLE_FX_NON_LOOPED_AT_COORD), aliens + 1, "and a flash, for the streamer and each alien");
        mock.set("inAir", 0);
        Check(WaitIdle(), "it ends when they land");

        Section("...under a roof");
        ResetState();
        ClearSpawned();
        mock.set("wallAhead", 1);
        mock.clearCalls();
        events.Clear();
        Fire("abduct");
        PumpFor(5);
        CheckEq(mock.count(H::SET_ENTITY_VELOCITY), 0, "it waits for open sky");
        mock.set("wallAhead", 0);
        Check(WaitFired("abduct"), "and goes once there is");
        WaitIdle();

        Section("...from a vehicle");
        Check(FireFreshWith("abduct", { { "inVehicle", 1 } }), "it fires in a car");
        CheckEq(mock.count(H::SET_VEHICLE_DOOR_OPEN), 4, "a closed car's doors pop open");
        CheckEq(mock.count(H::CLEAR_PED_TASKS_IMMEDIATELY), 1, "and the streamer is pulled out");
        WaitIdle();
        Check(FireFreshWith("abduct", { { "inVehicle", 1 }, { "bike", 1 } }), "and on a bike");
        CheckEq(mock.count(H::SET_VEHICLE_DOOR_OPEN), 0, "where there's nothing to open");
        CheckEq(mock.count(H::CLEAR_PED_TASKS_IMMEDIATELY), 1, "they're pulled off it");
        WaitIdle();

        Section("...with its parachute switched off");
        Http("POST", "/api/set", "key=AbductParachute&value=0");
        Check(FireFresh("abduct"), "it fires");
        mock.set("inAir", 1);
        mock.set("raisedBy", 26);
        PumpFor(3);
        CheckEq(mock.countArg(H::GIVE_WEAPON_TO_PED, 1, (int)Joaat("gadget_parachute")), 0, "there's no parachute");
        CheckEq(mock.count(H::FORCE_PED_TO_OPEN_PARACHUTE), 0, "to open");
        mock.set("inAir", 0);
        WaitIdle();
        Http("POST", "/api/set", "key=AbductParachute&value=1");

        Section("...when it's cut short");
        Check(FireFresh("abduct"), "it fires");
        Check(Pump([] { return mock.count(H::CREATE_PED) > 0; }, 3.0), "the aliens arrive");
        mock.set("dead", 1);
        PumpFor(2);
        Check(mock.count(H::CREATE_PED) >= 3 && mock.count(H::DELETE_PED) == mock.count(H::CREATE_PED), "the aliens go too");
        ResetState();
        PumpFor(1);
    }

    void TestControls()
    {
        Section("Streamer controls");
        ResetState();
        Chat("mod_user", "!give kim 100000", "mod");
        Balance("kim");

        events.Clear();
        mock.key(VK_F9);
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":false") != std::string::npos; }, 3.0), "the pause hotkey pauses");
        Chat("kim", "!buy fog");
        Check(Pump([] { return events.Contains("is paused right now"); }), "purchases are refused while paused");
        events.Clear();
        Fire("fog");
        Check(WaitFired("fog"), "the panel's Fire button still works while paused");
        WaitIdle();
        mock.key(VK_F9);
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":true") != std::string::npos; }, 3.0), "the pause hotkey resumes");

        // The pick is random, and a vehicle effect correctly waits while the player is on foot,
        // so either one shows the hotkey worked.
        events.Clear();
        mock.key(VK_F7);
        Check(Pump([] { return events.Contains("\"type\":\"fired\"") || Http("GET", "/state").body.find("\"queued\":0") == std::string::npos; }),
              "the chaos hotkey fires or queues something");
        Http("POST", "/api/control", "action=clear");
        WaitIdle();

        events.Clear();
        Chat("kim", "!cvs off");
        PumpFor(1);
        Check(Http("GET", "/state").body.find("\"enabled\":true") != std::string::npos, "viewers can't use !cvs");
        Chat("mod_user", "!cvs off", "mod");
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":false") != std::string::npos; }, 3.0), "mods can pause with !cvs off");
        Chat("mod_user", "!cvs on", "mod");
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":true") != std::string::npos; }, 3.0), "mods can resume with !cvs on");

        Http("POST", "/api/set", "key=ChatBonus&value=0"); // each !balance below would otherwise earn points

        // Purchases already queued when the streamer pauses wait for the resume.
        mock.set("cutscene", 1);
        PumpFor(1);
        Chat("kim", "!buy fog");
        Http("POST", "/api/control", "action=pause");
        mock.set("cutscene", 0);
        mock.clearCalls();
        PumpFor(5);
        CheckEq(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST), 0, "a queued purchase holds while paused");
        events.Clear();
        Http("POST", "/api/control", "action=resume");
        Check(WaitFired("fog"), "and fires on resume");
        WaitIdle();

        // Clearing the queue refunds.
        mock.set("cutscene", 1);
        PumpFor(1);
        long long before = Balance("kim");
        Chat("kim", "!buy fog");
        Chat("kim", "!buy storm");
        PumpFor(1);
        CheckEq(Balance("kim"), before - 250, "two purchases are held");
        Chat("mod_user", "!cvs clear", "mod");
        Check(Pump([] { return Http("GET", "/state").body.find("\"queued\":0") != std::string::npos; }, 3.0), "!cvs clear empties the queue");
        CheckEq(Balance("kim"), before, "and refunds them");
        mock.set("cutscene", 0);

        // Queue limit.
        mock.set("cutscene", 1);
        PumpFor(1);
        Http("POST", "/api/set", "key=QueueLimit&value=2");
        before = Balance("kim");
        events.Clear();
        Chat("kim", "!buy fog");
        Chat("kim", "!buy storm");
        Chat("kim", "!buy snow");
        Check(Pump([] { return events.Contains("queue is full, so nothing was charged"); }), "a full queue refuses");
        CheckEq(Balance("kim"), before - 250, "and doesn't charge for the refused one");
        Http("POST", "/api/control", "action=clear");
        Http("POST", "/api/set", "key=QueueLimit&value=25");
        Http("POST", "/api/set", "key=ChatBonus&value=5");
        mock.set("cutscene", 0);

        // Disabled effects.
        Http("POST", "/api/set", "key=E_fog&value=0");
        events.Clear();
        Chat("kim", "!buy fog");
        Check(Pump([] { return events.Contains("is switched off"); }), "a switched-off effect can't be bought");
        Check(Http("GET", "/state").body.find("\"id\":\"fog\"") == std::string::npos, "and leaves the shop");
        Http("POST", "/api/set", "key=E_fog&value=1");

        // Per-effect cooldown.
        Http("POST", "/api/set", "key=EffectCooldown&value=20");
        Http("POST", "/api/set", "key=MinSpacing&value=0");
        events.Clear();
        mock.clearCalls();
        Chat("kim", "!buy fog");
        Check(WaitFired("fog"), "first purchase fires");
        Chat("kim", "!buy fog");
        PumpFor(10);
        Check(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST) == 1, "the same effect waits out its cooldown");
        PumpFor(15);
        Check(mock.count(H::SET_WEATHER_TYPE_NOW_PERSIST) == 2, "and then fires");
        Http("POST", "/api/set", "key=EffectCooldown&value=0");

        // Chaos Gods auto-trigger.
        Http("POST", "/api/set", "key=ChaosGods&value=1");
        Http("POST", "/api/set", "key=ChaosGodsQuietMinutes&value=1");
        events.Clear();
        Check(Pump([] { return events.Contains("The Chaos Gods"); }, 6.0), "the Chaos Gods act when chat goes quiet");
        Http("POST", "/api/set", "key=ChaosGods&value=0");
        Http("POST", "/api/control", "action=clear");
        WaitIdle();

        // A random pick skips what can't run where the player is. Otherwise it sits in the queue
        // and is dropped two minutes later, which looked like F7 doing nothing.
        const auto idsWith = [](const std::string& flag)
        {
            std::vector<std::string> ids;
            const std::string body = Http("GET", "/api/settings").body, marker = "{\"id\":\"";
            for (size_t at = body.find("\"effects\":["); (at = body.find(marker, at)) != std::string::npos;)
            {
                at += marker.size();
                const std::string id = body.substr(at, body.find('"', at) - at);
                if (body.substr(at, body.find('}', at) - at).find("\"" + flag + "\":true") != std::string::npos) ids.push_back(id);
            }
            return ids;
        };
        for (int inVehicle = 0; inVehicle <= 1; ++inVehicle)
        {
            const std::vector<std::string> unrunnable = idsWith(inVehicle ? "needsOnFoot" : "needsVehicle");
            mock.set("inVehicle", inVehicle);
            PumpFor(1);
            events.Clear();
            for (int i = 0; i < 20; ++i)
            {
                Http("POST", "/api/control", "action=chaos");
                PumpFor(0.2);
            }
            Check(WaitIdle(), inVehicle ? "twenty random picks in a vehicle all run" : "twenty random picks on foot all run");
            bool skipped = !unrunnable.empty();
            for (auto& id : unrunnable)
                if (events.Contains("\"effect\":\"" + id + "\"")) skipped = false;
            Check(skipped, inVehicle ? "none of them needs the player on foot" : "none of them needs a vehicle");
        }
        mock.set("inVehicle", 0);
        Http("POST", "/api/control", "action=clear");
        WaitIdle();

        // Commands that just reply.
        events.Clear();
        Chat("kim", "!shop");
        Check(Pump([] { return events.Contains("See a tier with"); }), "!shop lists the tiers");
        Chat("kim", "!shop blessing");
        Check(Pump([] { return events.Contains("Blessing: clearsky"); }), "!shop <tier> lists prices");
        Chat("kim", "!info nuclear");
        Check(Pump([] { return events.Contains("Nuclear Chaos (Devastating, 50000 pts)"); }), "!info describes an effect");
    }

    void TestJunkChat()
    {
        Section("Junk chat can't hurt anything");
        const char* junk[] = {
            "", " ", "@", "@ ", "@a", "@a=b", ":", ": ", ":x", "PRIVMSG", "PRIVMSG #x", "PRIVMSG :", " :", "@x :y", "@x y z",
            ":!@ PRIVMSG #c :!buy", ":bad~login!x@y PRIVMSG #c :!buy fog", ":a!b@c PRIVMSG #c", "@display-name= :a!b@c PRIVMSG #c :!",
            "@bits=1e400;badges=subscriber/x :a!b@c PRIVMSG #c :hi", "@bits=-5 :a!b@c PRIVMSG #c :hi", "USERNOTICE", ":x USERNOTICE #c",
            "@msg-id=raid;login=ok_name;msg-param-viewerCount=nan :x USERNOTICE #c", "@msg-id=raid;login=bad%20name;msg-param-viewerCount=99 :x USERNOTICE #c",
            "@badges=broadcaster/1 :a!b@c PRIVMSG #c :!give", "@badges=broadcaster/1 :a!b@c PRIVMSG #c :!give x", "@badges=broadcaster/1 :a!b@c PRIVMSG #c :!give x 1e99",
            ":a!b@c PRIVMSG #c :!info", ":a!b@c PRIVMSG #c :!shop x y z", ":a!b@c PRIVMSG #c :!buy    ", ":a!b@c PRIVMSG #c :!cvs nope",
        };
        for (const char* line : junk)
            Check(Http("POST", "/api/simulate", "line=" + UrlEncode(line)).status == 200, std::string("survives: ") + line);

        Http("POST", "/api/control", "action=clear");
        Check(Http("GET", "/state").status == 200, "the server is still up");

        // Long and oversized input.
        Check(Http("POST", "/api/simulate", "line=" + UrlEncode(":a!b@c PRIVMSG #c :" + std::string(3000, 'x'))).status == 200, "a very long message is fine");
        // Either a 400 or a reset, depending on how much the server had read when it hung up.
        Check(Http("POST", "/api/simulate", "line=" + std::string(20000, 'x')).status != 200, "an oversized request is refused");
        Check(Http("GET", "/state").status == 200, "and the server carries on");

        PumpFor(1);
    }

    bool WriteTextFile(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::trunc | std::ios::binary);
        out << text;
        return (bool)out;
    }

    // What only the RAGE Plugin Hook build has to get right. Everything else is covered by
    // running the other tests against it.
    void TestRphBackend(HMODULE core)
    {
        Section("RAGE Plugin Hook build");
        Check(ImportsDll(core, "KERNEL32.dll"), "the import check can see imports at all");
        Check(!ImportsDll(core, "ScriptHookV.dll"), "it doesn't need ScriptHookV.dll, which RPH users won't have");

        ResetState();
        Check(rph.start(kCvsRphApiVersion + 1, &kRphHost, "test") != 0, "a plugin from a different build is refused");
        Check(rph.start(kCvsRphApiVersion, nullptr, "test") != 0, "starting without a host is refused");
        mock.clearCalls();
        events.Clear();
        Fire("fog");
        Check(WaitFired("fog"), "a refused start leaves the running plugin alone");
        WaitIdle();

        Section("...when the plugin unloads and loads again");
        ClearSpawned();
        events.Clear();
        Fire("sasquatch");
        WaitFired("sasquatch");
        Fire("plotarmor");
        WaitFired("plotarmor");
        rph.stop();
        mock.clearCalls();
        rph.tick();
        rph.tick();
        CheckEq(mock.totalCalls(), 0, "nothing calls the game after the plugin unloads");
        Sleep((DWORD)(6.0 / kTimeScale * 1000) + 50);
        Check(Http("POST", "/api/fire", "id=fog").status == 400, "the panel is told the game isn't running while it's unloaded");
        Check(Http("GET", "/state").status == 200, "the web server keeps running while it's unloaded");

        Check(rph.start(kCvsRphApiVersion, &kRphHost, "test") == 0, "it can load again");
        PumpFor(1);
        int invincible = -1;
        Check(mock.lastInt(H::SET_PLAYER_INVINCIBLE, 1, &invincible) && invincible == 0, "loading again puts back what an unfinished effect changed");
        CheckEq(mock.count(H::REMOVE_BLIP), 8, "and takes the blips off a swarm that's still standing");
        CheckEq(mock.count(H::RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER), 1, "and puts melee damage back");
        ClearSpawned();
        events.Clear();
        Fire("fog");
        Check(WaitFired("fog"), "effects work after it loads again");
        WaitIdle();

        Section("...when the game loads a save");
        // RPH keeps the script running through this, so the core has to notice for itself.
        ClearSpawned();
        mock.clearCalls();
        events.Clear();
        Fire("dogs");
        WaitFired("dogs");
        const int groups = mock.count(H::ADD_RELATIONSHIP_GROUP);
        events.Clear();
        Fire("dogs");
        WaitFired("dogs");
        CheckEq(mock.count(H::ADD_RELATIONSHIP_GROUP), groups, "the hostile group is reused during a session");

        events.Clear();
        Fire("plotarmor");
        WaitFired("plotarmor");
        mock.set("loading", 1);
        PumpFor(1);
        mock.set("loading", 0);
        PumpFor(1);
        invincible = -1;
        Check(mock.lastInt(H::SET_PLAYER_INVINCIBLE, 1, &invincible) && invincible == 0, "loading a save can't leave the player invincible");

        events.Clear();
        Fire("dogs");
        Check(WaitFired("dogs"), "effects work after loading a save");
        CheckEq(mock.count(H::ADD_RELATIONSHIP_GROUP), groups + 1, "a loaded save gets a new hostile group");
        WaitIdle();
        ClearSpawned();

        Section("...when a hotkey is held down");
        // Set directly, because RphFrame lets go of the key after one frame.
        g_rphKeyDown = VK_F9;
        for (int i = 0; i < 5; ++i) rph.tick();
        g_rphKeyDown = 0;
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":false") != std::string::npos; }, 3.0), "holding the pause key pauses");
        PumpFor(1);
        Check(Http("GET", "/state").body.find("\"enabled\":false") != std::string::npos, "holding the pause key only toggles once");
        mock.key(VK_F9);
        Check(Pump([] { return Http("GET", "/state").body.find("\"enabled\":true") != std::string::npos; }, 3.0), "pressing it again after letting go resumes");
    }

    int RunTwitchSmoke(const fs::path& asi, const fs::path& mockDll, const fs::path& data, const std::string& channel)
    {
        WriteTextFile(data / L"ChatVsStreamer.ini",
                      "; test\n[Twitch]\nChannel=" + channel + "\nBotUsername=justinfan" + std::to_string(10000 + GetCurrentProcessId() % 89999) +
                      "\nOAuthToken=oauth:anonymous\n\n[Ports]\nWeb=17373\n");
        HMODULE m = LoadLibraryW(mockDll.c_str());
        Bind(m, mock.init, "mock_init");
        Bind(m, mock.frame, "mock_frame");
        mock.init();
        if (!LoadLibraryW(asi.c_str())) { printf("Failed to load the plugin: %lu\n", GetLastError()); return 2; }

        printf("Connecting anonymously to #%s (read-only)...\n", channel.c_str());
        const ULONGLONG deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline)
        {
            mock.frame();
            std::ifstream in(data / L"ChatVsStreamer.log");
            std::stringstream text;
            text << in.rdbuf();
            if (text.str().find("Joined #" + channel) != std::string::npos)
            {
                printf("%s", text.str().c_str());
                printf("PASS: WebSocket connect, login and JOIN all work.\n");
                ExitProcess(0);
            }
            Sleep(100);
        }
        std::ifstream in(data / L"ChatVsStreamer.log");
        std::stringstream text;
        text << in.rdbuf();
        printf("%sFAIL: never joined the channel.\n", text.str().c_str());
        ExitProcess(1);
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { printf("usage: TestHost <build folder> <repo root> [--twitch-smoke channel]\n"); return 2; }
    // Absolute, because SetDllDirectory below takes the current folder out of the DLL search.
    const fs::path build = fs::absolute(argv[1]), repo = fs::absolute(argv[2]);

    setvbuf(stdout, nullptr, _IONBF, 0);

    // A throwaway data folder, so this can never touch a real ledger or real settings.
    wchar_t tempBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBuf);
    // The plugin stays loaded until the process dies, so a run can't delete its own
    // folder. Each run clears out what earlier runs left behind instead.
    std::error_code ignored;
    for (auto& entry : fs::directory_iterator(tempBuf, ignored))
        if (entry.path().filename().wstring().rfind(L"cvs-test-", 0) == 0) fs::remove_all(entry.path(), ignored);
    const fs::path data = fs::path(tempBuf) / (L"cvs-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(data);
    SetEnvironmentVariableW(L"CVS_DATA_DIR", data.c_str());

    fs::copy_file(repo / L"mod" / L"overlay.html", data / L"overlay.html");
    fs::copy_file(repo / L"mod" / L"panel.html", data / L"panel.html");

    const fs::path mockDll = build / L"mock" / L"Release" / L"ScriptHookV.dll";
    const fs::path asi = build / L"Release" / L"ChatVsStreamerGTAV.asi";

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (argc >= 5 && std::wstring(argv[3]) == L"--twitch-smoke")
        return RunTwitchSmoke(asi, mockDll, data, Narrow(argv[4]));

    const bool serve = argc >= 4 && std::wstring(argv[3]) == L"--serve";
    const bool useRph = argc >= 4 && std::wstring(argv[3]) == L"--rph";
    if (serve) g_port = kServePort;
    else SetEnvironmentVariableW(L"CVS_TEST_TIMESCALE", std::to_wstring((int)kTimeScale).c_str());
    if (useRph) g_port = kRphPort;

    // A channel and bot are named so the leaderboard has someone to leave out, but with no token
    // the mod doesn't connect to Twitch. The bot's name has no "bot" in it, so that rule can't
    // be what hides it.
    WriteTextFile(data / L"ChatVsStreamer.ini", "; test\n[Twitch]\nChannel=TestStreamer\nBotUsername=ChatHelper\nOAuthToken=\n\n[Ports]\nWeb=" + std::to_string(g_port) + "\n");
    std::error_code noHiddenFile;
    fs::remove(data / L"leaderboard_hidden.txt", noHiddenFile); // so the test sees the mod create it
    if (serve)
    {
        // A demo of the panel and overlay at normal speed, with no game and no Twitch.
        WriteTextFile(data / L"ledger.tsv", "chaos_fan\tChaosFan\t12400\t1\nsunset_rider\tSunsetRider\t8150\t0\nlate_night_lurker\tLateNightLurker\t3320\t0\n"
                                            "test_viewer\ttest_viewer\t100000\t0\n");
    }
    else
    {
        WriteTextFile(data / L"ledger.tsv", "seeded_viewer\tSeededViewer\t777\t0\n");
        // No spacing or cooldowns by default, so tests don't wait on them. Tests that care set them.
        WriteTextFile(data / L"settings.ini", "; test\nMinSpacing=0\nEffectCooldown=0\n");
    }

    // The DLL search path must find the mock first, the same way the game folder would.
    SetDllDirectoryW((build / L"mock" / L"Release").c_str());
    HMODULE m = LoadLibraryW(mockDll.c_str());
    if (!m) { printf("Couldn't load the mock: %lu\n", GetLastError()); return 2; }
    Bind(m, mock.init, "mock_init");
    Bind(m, mock.frame, "mock_frame");
    Bind(m, mock.restart, "mock_restart_script");
    Bind(m, mock.set, "mock_set");
    Bind(m, mock.get, "mock_get");
    Bind(m, mock.clearCalls, "mock_clear_calls");
    Bind(m, mock.clearModels, "mock_clear_models");
    Bind(m, mock.count, "mock_count");
    Bind(m, mock.totalCalls, "mock_total_calls");
    Bind(m, mock.lastInt, "mock_last_arg_int");
    Bind(m, mock.lastFloat, "mock_last_arg_float");
    Bind(m, mock.nthFloat, "mock_nth_arg_float");
    Bind(m, mock.fedControl, "mock_fed_control");
    Bind(m, mock.lastText, "mock_last_text");
    Bind(m, mock.countActions, "mock_count_actions");
    Bind(m, mock.key, "mock_key");
    Bind(m, mock.countArg, "mock_count_arg");
    mock.init();
    ResetState();

    HMODULE plugin = nullptr;
    if (useRph)
    {
        printf("Testing the RAGE Plugin Hook build.\n");
        plugin = LoadLibraryW((build / L"Release" / L"ChatVsStreamerCore.dll").c_str());
        if (!plugin) return ReportBlockedOrFailedLoad("ChatVsStreamerCore.dll", GetLastError());
        Bind(plugin, rph.start, "cvs_rph_start");
        Bind(plugin, rph.tick, "cvs_rph_tick");
        Bind(plugin, rph.stop, "cvs_rph_stop");
        Bind(m, g_mockCall, "mock_call");
        Bind(m, g_mockVehicles, "mock_vehicles");
        Bind(m, g_mockPeds, "mock_peds");
        mock.frame = RphFrame;
        mock.restart = RphReload;
        mock.key = RphKey;
        if (rph.start(kCvsRphApiVersion, &kRphHost, "test") != 0) { printf("ChatVsStreamerCore.dll refused to start.\n"); return 2; }
    }
    else
    {
        plugin = LoadLibraryW(asi.c_str());
        if (!plugin) return ReportBlockedOrFailedLoad("ChatVsStreamerGTAV.asi", GetLastError());
    }

    if (!Pump([] { return Http("GET", "/state").status == 200; }, 8.0)) { printf("The web server never came up.\n"); return 2; }

    if (serve)
    {
        printf("Simulated game running.\n  Panel:   http://localhost:%d/panel\n  Overlay: http://localhost:%d/\nThe simulated player gets in and out of a vehicle every 15 seconds. Ctrl+C to stop.\n", g_port, g_port);
        for (ULONGLONG frame = 0;; ++frame)
        {
            mock.set("inVehicle", (int)((frame / (60 * 15)) % 2));
            mock.frame();
            if (frame % 600 == 0) mock.clearCalls(); // the mock records every call, so keep that from growing forever
            Sleep(16);
        }
    }

    events.Start();
    Pump([] { return Http("GET", "/state").body.find("\"game\":true") != std::string::npos; }, 4.0);

    TestHashes();
    TestText();
    TestJson();
    TestWebSecurity();
    TestSettings(data);
    TestEconomy(data);
    TestRewards(data);
    TestGating();
    TestSpawning();
    TestRestores();
    TestEveryEffect();
    TestFeedAndPlate();
    TestDroppingAWeapon();
    TestMoonGravity();
    TestChaosEffects();
    TestSwarms();
    TestMorph();
    TestWasted();
    TestAbduction();
    TestDirectCommands();
    TestControls();
    TestJunkChat();
    if (useRph) TestRphBackend(plugin);

    printf("\n%d checks, %d failed.\n", g_checks, g_failures);
    fflush(stdout);
    // The plugin is pinned and its threads are still running, so leave the hard way.
    ExitProcess(g_failures ? 1 : 0);
}
