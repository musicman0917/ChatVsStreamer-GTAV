#include "Settings.h"
#include "Effects.h"
#include "Log.h"
#include "Util.h"

#include <climits>
#include <fstream>
#include <sstream>

namespace cvs
{
    namespace
    {
        constexpr int kMax = 10000000;

        constexpr const char* kIniTemplate =
            "; Chat vs Streamer for GTA V: boot settings. Keep this file off stream, it holds your token.\n"
            "; Prices, toggles and economy settings are in the control panel: http://localhost:7373/panel\n"
            "\n"
            "[Twitch]\n"
            "; Your channel name (the chat the mod reads)\n"
            "Channel=\n"
            "; Account the mod replies as. Can be the same as Channel.\n"
            "BotUsername=\n"
            "; OAuth token for BotUsername with chat:read and chat:edit scopes\n"
            "OAuthToken=\n"
            "; Optional, so channel point rewards give chaos points: a token for your channel's own account\n"
            "; (not the bot's) with the channel:read:redemptions scope. The rewards are in channel_point_rewards.txt.\n"
            "BroadcasterToken=\n"
            "\n"
            "[Ports]\n"
            "; Overlay: http://localhost:<Web>/   Control panel: http://localhost:<Web>/panel\n"
            "Web=7373\n";

        std::string ReadIni(const std::wstring& path, const wchar_t* section, const wchar_t* key)
        {
            wchar_t buf[1024]{};
            GetPrivateProfileStringW(section, key, L"", buf, 1024, path.c_str());
            return Trim(Narrow(buf));
        }

        std::filesystem::path SettingsPath() { return DataDir() / L"settings.ini"; }
    }

    const std::vector<SettingDef>& SettingDefs()
    {
        static const std::vector<SettingDef> defs = {
            // key, default, group, label, help, isBool, min, max
            { "Enabled", 1, "Effects", "Chat can buy effects", "Off pauses all purchases. The pause hotkey flips this too.", true, 0, 1 },
            { "Alerts", 1, "Effects", "Show purchases in the game feed", "A ticker above the minimap names who bought what.", true, 0, 1 },
            { "BlockRiskyInMissions", 1, "Effects", "Hold risky effects during missions", "Teleports, vehicle swaps and clock changes can break a mission. They wait for it to end, or refund.", true, 0, 1 },
            { "EffectCooldown", 20, "Effects", "Cooldown per effect (seconds)", "How long before the same effect can fire again.", false, 0, 3600 },
            { "MinSpacing", 3, "Effects", "Gap between any two effects (seconds)", "Stops a queue from firing everything at once.", false, 0, 600 },
            { "QueueLimit", 25, "Effects", "Queue limit", "Purchases past this are refused before any points are spent.", false, 1, 200 },
            { "EjectParachute", 1, "Effects", "Ejector Seat gives a parachute", "Off means whoever gets ejected has to survive the landing, unless they already own one.", true, 0, 1 },
            { "AbductParachute", 1, "Effects", "Alien Abduction opens a parachute", "Off drops whoever gets abducted from 25m with nothing to break the fall.", true, 0, 1 },
            { "FedsClearSeconds", 0, "Effects", "Five-Star Sting clears after (seconds)", "0 leaves the cops on the streamer until they lose them.", false, 0, 600 },

            { "StartingPoints", 100, "Economy", "Starting points", "What a new viewer begins with.", false, 0, kMax },
            { "ChatBonus", 5, "Economy", "Points per chat message", "Awarded at most once per chat cooldown.", false, 0, kMax },
            { "ChatCooldown", 60, "Economy", "Chat cooldown (seconds)", "Minimum time between chat bonuses for a viewer.", false, 0, 86400 },
            { "PassivePoints", 10, "Economy", "Passive points", "For anyone who chatted recently. Subs earn 1.5x, 2x or 3x by tier.", false, 0, kMax },
            { "PassiveMinutes", 5, "Economy", "Passive interval (minutes)", "How often passive points pay out.", false, 1, 1440 },
            { "SubBonus", 100, "Economy", "Points for a sub or resub", "", false, 0, kMax },
            { "GiftBonus", 50, "Economy", "Points per gifted sub", "Paid to the person gifting.", false, 0, kMax },
            { "PointsPerBit", 1, "Economy", "Points per bit", "", false, 0, kMax },
            { "RaidPerViewer", 2, "Economy", "Raid points per raider", "Paid to the raid leader.", false, 0, kMax },

            { "RaidAttack", 1, "Events", "Raids send a hostile mob", "A raid spawns attackers: one per five raiders, between 3 and 12.", true, 0, 1 },
            { "RaidAttackMinViewers", 5, "Events", "Smallest raid that attacks", "", false, 1, 100000 },
            { "ChaosGods", 0, "Events", "Chaos Gods auto-trigger", "When chat has been quiet, the Chaos Gods fire a random effect.", true, 0, 1 },
            { "ChaosGodsQuietMinutes", 10, "Events", "Chaos Gods quiet time (minutes)", "No purchases for this long sets them off.", false, 1, 1440 },
            { "ChaosGodsPainful", 0, "Events", "Chaos Gods can use Painful effects", "Off keeps them to Nuisance and Disruptive.", true, 0, 1 },

            { "HotkeyChaos", 0x76, "Hotkeys", "Fire a random effect (key code)", "Windows key code: F1=112 ... F7=118, F9=120, F12=123. 0 turns it off.", false, 0, 255 },
            { "HotkeyPause", 0x78, "Hotkeys", "Pause or resume purchases (key code)", "Default F9. F8 is left alone because other popular trainers use it.", false, 0, 255 },
        };
        return defs;
    }

    BootConfig BootConfig::Load()
    {
        auto path = DataDir() / L"ChatVsStreamer.ini";
        if (!std::filesystem::exists(path))
        {
            std::ofstream(path) << kIniTemplate;
            log::Warn("Created {}. Fill in your Twitch details.", path.string());
        }

        auto p = path.wstring();
        BootConfig c;
        c.channel = Lower(ReadIni(p, L"Twitch", L"Channel"));
        c.botUsername = Lower(ReadIni(p, L"Twitch", L"BotUsername"));
        c.oauthToken = ReadIni(p, L"Twitch", L"OAuthToken");
        c.broadcasterToken = ReadIni(p, L"Twitch", L"BroadcasterToken");
        c.webPort = ToInt(ReadIni(p, L"Ports", L"Web"), 7373);

        if (StartsWith(c.channel, "#")) c.channel.erase(0, 1);
        if (c.botUsername.empty()) c.botUsername = c.channel;
        if (!c.oauthToken.empty() && !StartsWith(c.oauthToken, "oauth:")) c.oauthToken = "oauth:" + c.oauthToken;
        return c;
    }

    bool LiveSettings::Normalize(const std::string& key, int& value)
    {
        if (StartsWith(key, "P_") && FindEffect(key.substr(2)))
        {
            value = std::clamp(value, 1, kMax);
            return true;
        }
        if (StartsWith(key, "E_") && FindEffect(key.substr(2)))
        {
            value = value ? 1 : 0;
            return true;
        }
        for (auto& d : SettingDefs())
        {
            if (key != d.key) continue;
            value = d.isBool ? (value ? 1 : 0) : std::clamp(value, d.min, d.max);
            return true;
        }
        return false;
    }

    void LiveSettings::ApplyDefaults()
    {
        for (auto& d : SettingDefs()) m_values[d.key] = d.def;
        for (auto& e : AllEffects())
        {
            m_values[std::string("P_") + e.id] = e.defaultPrice;
            m_values[std::string("E_") + e.id] = 1;
        }
    }

    void LiveSettings::LoadOrCreate()
    {
        {
            std::lock_guard lock(m_mutex);
            ApplyDefaults();
        }
        if (std::filesystem::exists(SettingsPath())) ReadFile();
        WriteFile(); // adds any settings a newer version introduced, keeps the rest
    }

    void LiveSettings::ReadFile()
    {
        std::ifstream in(SettingsPath());
        std::map<std::string, int> parsed;
        std::string line;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            int value = ToInt(Trim(line.substr(eq + 1)), INT_MIN);
            if (value == INT_MIN || !Normalize(key, value)) continue;
            parsed[key] = value;
        }

        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(SettingsPath(), ec);
        {
            std::lock_guard fileLock(m_fileMutex);
            if (!ec) m_fileTime = stamp;
        }
        std::lock_guard lock(m_mutex);
        for (auto& [k, v] : parsed) m_values[k] = v;
    }

    void LiveSettings::WriteFile()
    {
        std::ostringstream text;
        text << "; Chat vs Streamer for GTA V: gameplay settings. Saved by the control panel at\n"
                "; http://localhost:7373/panel. You can edit this by hand too; changes apply within\n"
                "; a couple of seconds. P_<effect> is a price, E_<effect> is 1 for on and 0 for off.\n";
        {
            std::lock_guard lock(m_mutex);
            for (auto& d : SettingDefs()) text << d.key << '=' << m_values[d.key] << '\n';
            text << '\n';
            for (auto& e : AllEffects())
            {
                text << "P_" << e.id << '=' << m_values[std::string("P_") + e.id] << '\n';
                text << "E_" << e.id << '=' << m_values[std::string("E_") + e.id] << '\n';
            }
        }

        std::lock_guard fileLock(m_fileMutex);
        auto tmp = SettingsPath();
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
            out << text.str();
            if (!out) { log::Error("Failed to write settings.ini"); return; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, SettingsPath(), ec);
        if (ec) { log::Error("Failed to replace settings.ini: {}", ec.message()); return; }
        m_fileTime = std::filesystem::last_write_time(SettingsPath(), ec);
    }

    void LiveSettings::ReloadIfChanged()
    {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(SettingsPath(), ec);
        if (ec) return;
        {
            std::lock_guard fileLock(m_fileMutex);
            if (stamp == m_fileTime) return;
        }
        ReadFile();
        log::Info("Reloaded settings.ini");
    }

    bool LiveSettings::Set(const std::string& key, int value)
    {
        if (!Normalize(key, value)) return false;
        {
            std::lock_guard lock(m_mutex);
            m_values[key] = value;
        }
        WriteFile();
        return true;
    }

    int LiveSettings::Get(const char* key) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_values.find(key);
        return it == m_values.end() ? 0 : it->second;
    }

    int LiveSettings::Price(const std::string& effectId, int fallback) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_values.find("P_" + effectId);
        return it == m_values.end() ? fallback : it->second;
    }

    bool LiveSettings::EffectEnabled(const std::string& effectId) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_values.find("E_" + effectId);
        return it == m_values.end() || it->second != 0;
    }
}
