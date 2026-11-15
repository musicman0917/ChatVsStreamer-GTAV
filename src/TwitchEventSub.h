#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <Windows.h>
#include <winhttp.h>

namespace cvs
{
    struct Redemption
    {
        std::string id;          // Twitch's id for this redemption, to drop repeats
        std::string login;
        std::string display;
        std::string rewardTitle;
        int cost = 0;            // in channel points
        bool simulated = false;  // came from the control panel or the tests, not from Twitch
    };

    // Channel point redemptions from Twitch EventSub over WebSocket
    // (wss://eventsub.wss.twitch.tv/ws), signed up for through the Helix API.
    //
    // Twitch only shares redemptions with the channel's own account, so this needs that
    // account's token with the channel:read:redemptions scope. The chat bot's token won't
    // do unless the bot is the channel. Helix's own list of redemptions only covers rewards
    // made by the same app, but EventSub sees every reward, including ones made on Twitch's
    // dashboard or in Streamer.bot.
    class TwitchEventSub
    {
    public:
        std::function<void(const Redemption&)> onRedemption;

        void Start(std::string channel, std::string token);
        void Stop();
        bool IsListening() const { return m_listening; }

        // Runs one raw EventSub message through the normal handler, for the panel and the
        // tests. A redemption in it is flagged simulated, and session messages are ignored.
        void InjectMessage(const std::string& json);

    private:
        void Run();
        void Watchdog();
        bool CheckToken();
        bool ConnectOnce();
        void CloseHandles();
        bool Subscribe(const std::string& sessionId);
        void HandleMessage(const std::string& json, bool simulated);
        bool FirstTime(const std::string& id);

        std::string m_channel, m_token, m_clientId, m_userId;
        std::thread m_thread, m_watchdog;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_listening{ false };    // subscribed, so redemptions will arrive
        std::atomic<bool> m_giveUp{ false };       // the token can't work, so retrying is pointless
        std::atomic<bool> m_reconnectNow{ false }; // Twitch asked for a new connection
        std::atomic<double> m_lastMessage{ 0 };
        std::atomic<int> m_keepaliveSec{ 30 };

        HINTERNET m_session = nullptr, m_connect = nullptr, m_request = nullptr, m_socket = nullptr;
        std::mutex m_handleMutex;

        std::mutex m_seenMutex;
        std::deque<std::string> m_seenOrder;
        std::set<std::string> m_seen;
    };
}
