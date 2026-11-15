#pragma once

#include "EffectEngine.h"
#include "Economy.h"
#include "Settings.h"
#include "TwitchChat.h"
#include "TwitchEventSub.h"
#include "WebServer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cvs
{
    inline constexpr const char* kVersion = "0.1.0";

    struct Purchase
    {
        std::string effectId;
        std::string login;       // empty for panel tests and the Chaos Gods, which nobody pays for
        std::string display;
        long long paid = 0;
        double queuedAt = 0;
        double nextAttempt = 0;
        int count = 0;           // size override (a raid's mob)
        bool force = false;      // the streamer's own trigger: skips the pause, cooldown and "disabled" checks
        bool simulated = false;  // replies go to the panel, not to real chat
        std::string waitReason;  // why it's still waiting, for the refund message
    };

    // Threading, the one rule that matters: natives run only on the script fiber, and
    // the script fiber never blocks on the network or disk.
    //
    //   Twitch thread, web threads  -> push Purchases onto m_queue
    //   Script fiber (ScriptTick)   -> pops them, calls natives, pushes Outcomes
    //   Worker thread               -> refunds, chat replies, saving, passive points
    class Mod
    {
    public:
        void Start();                // on a background thread at load
        void Stop();
        void OnScriptStart();        // on the script fiber, each time Script Hook V (re)starts it
        void ScriptTick();           // on the script fiber, once per frame
        void OnKey(DWORD key, bool wasDown, bool isUp); // on the game's window thread
        // For a host with no keyboard callback (RAGE Plugin Hook). On the script fiber, once per frame.
        void PollHotkeys(bool (*isDown)(int virtualKey));

        std::string hostGameVersion; // RAGE Plugin Hook build only, set before Start()

    private:
        struct Outcome
        {
            Purchase purchase;
            EffectResult::Kind kind = EffectResult::Kind::Ok;
            std::string reason;
        };

        // Chat
        void OnChatMessage(const ChatMessage& msg);
        void OnChatEvent(const ChatEvent& ev);
        void HandleCommand(const ChatMessage& msg, const std::string& cmd, const std::vector<std::string>& args);
        void CmdBuy(const ChatMessage& msg, const std::string& effectId);
        void CmdGive(const ChatMessage& msg, const std::vector<std::string>& args);
        void CmdShop(const ChatMessage& msg, const std::vector<std::string>& args);
        void CmdInfo(const ChatMessage& msg, const std::string& effectId);
        void CmdControl(const ChatMessage& msg, const std::vector<std::string>& args);
        void Reply(bool simulated, const std::string& text);
        void OnRedemption(const Redemption& r);

        // Queue
        bool Enqueue(Purchase purchase, size_t* positionOut = nullptr);
        size_t ClearQueue();
        bool FireChaosGods();
        void SetEnabled(bool enabled);
        bool GameAlive() const;

        // Script fiber
        void ServiceQueue(double now, const EffectEngine::Status& status);
        void HandleHotkeys();
        void PostFeed(const std::string& text);

        // Worker thread
        void WorkerLoop();
        void HandleOutcome(const Outcome& outcome);
        void Alert(const std::string& kind, const std::string& viewer, const std::string& text);
        void ReloadHiddenIfChanged();
        void ReloadRewardsIfChanged();

        // Web
        HttpReply HandleHttp(const HttpRequest& req);
        std::string StateJson();
        std::string SettingsJson();

        BootConfig m_boot;
        LiveSettings m_settings;
        std::unique_ptr<Economy> m_economy;
        EffectEngine m_engine;
        TwitchChat m_chat;
        TwitchEventSub m_eventSub;
        WebServer m_web;

        std::thread m_worker;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_started{ false };

        std::mutex m_queueMutex;
        std::deque<Purchase> m_queue;

        std::mutex m_outcomeMutex;
        std::condition_variable m_outcomeCv;
        std::deque<Outcome> m_outcomes;

        // Accounts left off the leaderboard, from leaderboard_hidden.txt.
        std::mutex m_hiddenMutex;
        std::set<std::string> m_hidden;
        std::filesystem::file_time_type m_hiddenTime{};

        // Channel point rewards that give points, from channel_point_rewards.txt: lowercase title -> points.
        std::mutex m_rewardsMutex;
        std::map<std::string, long long> m_rewards;
        std::filesystem::file_time_type m_rewardsTime{};

        // Script fiber only.
        double m_lastFire = 0;
        double m_lastChaosCheck = 0;
        std::map<std::string, double> m_lastFired;
        bool m_chaosKeyWasDown = false;
        bool m_pauseKeyWasDown = false;

        std::atomic<double> m_lastTick{ 0 };
        std::atomic<double> m_lastActivity{ 0 };
        std::atomic<int> m_block{ (int)Block::Loading };
        std::atomic<bool> m_inVehicle{ false };
        std::atomic<bool> m_onMission{ false };
        std::atomic<bool> m_hotkeyChaos{ false };
        std::atomic<bool> m_hotkeyPause{ false };
        std::atomic<int> m_pendingEnabled{ -1 };
    };
}
