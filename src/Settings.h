#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cvs
{
    // ChatVsStreamer.ini: secrets and ports, read once at startup. The control panel
    // never reads or writes this file, so the OAuth token can't leak through it.
    struct BootConfig
    {
        std::string channel;
        std::string botUsername;
        std::string oauthToken;
        std::string broadcasterToken; // optional: the channel's own token, for channel point rewards
        int webPort = 7373;

        static BootConfig Load();
        bool TwitchConfigured() const { return !channel.empty() && !oauthToken.empty(); }
    };

    struct SettingDef
    {
        const char* key;
        int def;
        const char* group;
        const char* label;
        const char* help;
        bool isBool;
        int min;
        int max;
    };

    const std::vector<SettingDef>& SettingDefs();

    // settings.ini: every price, toggle and economy number. GTA V has no in-game mod
    // menu, so this file is edited through http://localhost:7373/panel or by hand.
    // Changes to the file apply within a couple of seconds, without a restart.
    class LiveSettings
    {
    public:
        void LoadOrCreate();
        void ReloadIfChanged();

        // Validates and clamps, then saves. Returns false for an unknown key.
        bool Set(const std::string& key, int value);

        int Get(const char* key) const;
        int Price(const std::string& effectId, int fallback) const;
        bool EffectEnabled(const std::string& effectId) const;

    private:
        static bool Normalize(const std::string& key, int& value);
        void ApplyDefaults();
        void ReadFile();
        void WriteFile();

        mutable std::mutex m_mutex;
        std::map<std::string, int> m_values;
        std::mutex m_fileMutex;
        std::filesystem::file_time_type m_fileTime{};
    };
}
