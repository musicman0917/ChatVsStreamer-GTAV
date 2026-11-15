#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cvs
{
    class LiveSettings;

    struct Viewer
    {
        std::string login;      // lowercase key
        std::string display;
        long long points = 0;
        int subTier = 0;        // 0 none, 1-3 Twitch tiers (Prime counts as 1)
        double lastChat = 0;    // NowSeconds() of last message (session only)
        double lastChatBonus = 0;
    };

    // Viewer points ledger, persisted to ledger.tsv in the data folder. Keys are
    // Twitch logins today; a future platform source should prefix its ids ("tt:name")
    // so viewers on different platforms can't collide.
    class Economy
    {
    public:
        explicit Economy(const LiveSettings& settings) : m_settings(settings) {}

        void Load();
        void SaveIfDirty();

        // Records a chat line and awards the chat bonus when off cooldown.
        void OnChat(const std::string& login, const std::string& display, int subTier);
        // Passive income for everyone who chatted recently. Call once per interval.
        void PassiveTick();

        long long Balance(const std::string& login);
        void Add(const std::string& login, const std::string& display, long long amount);
        // Returns false (and changes nothing) if the viewer can't afford it.
        bool TrySpend(const std::string& login, long long amount);

        // Highest balances first. Leaves out the panel's test viewers, and any login `hide` picks out.
        std::vector<Viewer> Top(size_t count, const std::function<bool(const std::string& login)>& hide = nullptr);

    private:
        Viewer& Get(const std::string& login, const std::string& display);
        static double Multiplier(int subTier);

        const LiveSettings& m_settings;
        std::mutex m_mutex;
        std::map<std::string, Viewer> m_viewers;
        bool m_dirty = false;
    };
}
