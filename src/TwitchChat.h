#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <Windows.h>
#include <winhttp.h>

namespace cvs
{
    struct ChatMessage
    {
        std::string login;
        std::string display;
        std::string text;
        std::string color;
        int subTier = 0;
        int bits = 0;
        bool isMod = false;
        bool isBroadcaster = false;
        bool simulated = false; // came from the control panel, not from Twitch
    };

    // Subs, resubs, gift subs and raids (IRC USERNOTICE).
    struct ChatEvent
    {
        std::string type;       // "sub", "resub", "subgift", "submysterygift", "raid"
        std::string login;
        std::string display;
        int subTier = 0;
        int count = 0;          // gift count or raid viewer count
        bool simulated = false;
    };

    // Twitch chat over WebSocket IRC (wss://irc-ws.chat.twitch.tv), using WinHTTP
    // so the plugin needs no third-party networking libraries.
    class TwitchChat
    {
    public:
        std::function<void(const ChatMessage&)> onMessage;
        std::function<void(const ChatEvent&)> onEvent;

        void Start(std::string channel, std::string nick, std::string token);
        void Stop();
        void Say(const std::string& text);
        bool IsConnected() const { return m_connected; }

        // Runs one raw IRC line through the normal parser, for the panel's chat
        // simulator. Anything it triggers is flagged simulated and never reaches Twitch.
        void InjectLine(const std::string& line);

    private:
        void Run();
        bool ConnectOnce();
        void CloseHandles();
        bool SendRaw(const std::string& line);
        void HandleLine(const std::string& line, bool simulated);
        void FlushOutbox();

        std::string m_channel, m_nick, m_token;
        std::thread m_thread, m_sender;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_connected{ false };

        HINTERNET m_session = nullptr, m_connect = nullptr, m_request = nullptr, m_socket = nullptr;
        std::mutex m_sendMutex;

        std::mutex m_outboxMutex;
        std::deque<std::string> m_outbox;
    };
}
