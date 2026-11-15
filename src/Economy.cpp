#include "Economy.h"
#include "Log.h"
#include "Settings.h"
#include "Util.h"

#include <fstream>
#include <sstream>

namespace cvs
{
    namespace
    {
        std::filesystem::path LedgerPath() { return DataDir() / L"ledger.tsv"; }

        // The ledger is tab-separated, so a display name can't contain tabs or newlines.
        std::string CleanDisplay(std::string s)
        {
            for (char& c : s)
                if (c == '\t' || c == '\r' || c == '\n') c = ' ';
            return s;
        }
    }

    void Economy::Load()
    {
        std::lock_guard lock(m_mutex);
        std::ifstream in(LedgerPath());
        std::string line;
        while (std::getline(in, line))
        {
            auto cols = Split(line, '\t');
            if (cols.size() < 3) continue;
            Viewer v;
            v.login = cols[0];
            v.display = cols[1];
            try { v.points = std::stoll(cols[2]); }
            catch (...) { log::Warn("Skipping bad ledger line: {}", line); continue; }
            if (cols.size() > 3) v.subTier = ToInt(cols[3], 0);
            m_viewers[v.login] = v;
        }
        log::Info("Loaded {} viewers from ledger", m_viewers.size());
    }

    void Economy::SaveIfDirty()
    {
        // Build the file text under the lock but write it outside, so a slow disk
        // can't stall chat threads waiting on the same mutex.
        std::string text;
        {
            std::lock_guard lock(m_mutex);
            if (!m_dirty) return;
            std::ostringstream out;
            for (auto& [login, v] : m_viewers)
                out << v.login << '\t' << v.display << '\t' << v.points << '\t' << v.subTier << '\n';
            text = out.str();
            m_dirty = false;
        }

        auto tmp = LedgerPath();
        tmp += L".tmp";
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
            out << text;
            ok = (bool)out;
        }
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, LedgerPath(), ec);
        if (!ok || ec)
        {
            log::Error("Failed to save ledger: {}", ec ? ec.message() : "write failed");
            std::lock_guard lock(m_mutex);
            m_dirty = true; // try again next interval
        }
    }

    Viewer& Economy::Get(const std::string& login, const std::string& display)
    {
        auto [it, inserted] = m_viewers.try_emplace(login);
        Viewer& v = it->second;
        if (inserted)
        {
            v.login = login;
            v.points = m_settings.Get("StartingPoints");
            m_dirty = true;
        }
        if (!display.empty() && v.display != display) { v.display = CleanDisplay(display); m_dirty = true; }
        if (v.display.empty()) v.display = login;
        return v;
    }

    double Economy::Multiplier(int subTier)
    {
        switch (subTier)
        {
        case 1: return 1.5;
        case 2: return 2.0;
        case 3: return 3.0;
        default: return 1.0;
        }
    }

    void Economy::OnChat(const std::string& login, const std::string& display, int subTier)
    {
        std::lock_guard lock(m_mutex);
        Viewer& v = Get(login, display);
        double now = NowSeconds();
        v.lastChat = now;
        if (v.subTier != subTier) { v.subTier = subTier; m_dirty = true; }

        if (now - v.lastChatBonus >= m_settings.Get("ChatCooldown"))
        {
            v.lastChatBonus = now;
            v.points += m_settings.Get("ChatBonus");
            m_dirty = true;
        }
    }

    void Economy::PassiveTick()
    {
        std::lock_guard lock(m_mutex);
        double now = NowSeconds();
        // "Watching" = chatted within the last three passive intervals. Twitch's
        // chatters list needs a moderator-scoped Helix token; this avoids it.
        double window = m_settings.Get("PassiveMinutes") * 60.0 * 3;
        int awarded = 0;
        for (auto& [login, v] : m_viewers)
        {
            if (v.lastChat <= 0 || now - v.lastChat > window) continue;
            v.points += (long long)(m_settings.Get("PassivePoints") * Multiplier(v.subTier));
            ++awarded;
        }
        if (awarded) { m_dirty = true; log::Info("Passive points awarded to {} viewers", awarded); }
    }

    long long Economy::Balance(const std::string& login)
    {
        std::lock_guard lock(m_mutex);
        return Get(login, "").points;
    }

    void Economy::Add(const std::string& login, const std::string& display, long long amount)
    {
        std::lock_guard lock(m_mutex);
        Get(login, display).points += amount;
        m_dirty = true;
    }

    bool Economy::TrySpend(const std::string& login, long long amount)
    {
        std::lock_guard lock(m_mutex);
        Viewer& v = Get(login, "");
        if (v.points < amount) return false;
        v.points -= amount;
        m_dirty = true;
        return true;
    }

    std::vector<Viewer> Economy::Top(size_t count, const std::function<bool(const std::string&)>& hide)
    {
        std::lock_guard lock(m_mutex);
        std::vector<Viewer> all;
        for (auto& [login, v] : m_viewers)
        {
            if (StartsWith(login, "test_")) continue; // the panel's test viewers stay off the leaderboard
            if (hide && hide(login)) continue;
            all.push_back(v);
        }
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.points > b.points; });
        if (all.size() > count) all.resize(count);
        return all;
    }
}
