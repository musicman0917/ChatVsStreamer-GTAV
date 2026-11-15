#include "TwitchChat.h"
#include "Log.h"
#include "Util.h"

namespace cvs
{
    namespace
    {
        // IRCv3 tag values escape ; space \ CR LF.
        std::string UnescapeTag(const std::string& v)
        {
            std::string out;
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (v[i] != '\\' || i + 1 >= v.size()) { out.push_back(v[i]); continue; }
                switch (v[++i])
                {
                case 's': out.push_back(' '); break;
                case ':': out.push_back(';'); break;
                case 'r': out.push_back('\r'); break;
                case 'n': out.push_back('\n'); break;
                default: out.push_back(v[i]); break;
                }
            }
            return out;
        }

        struct IrcLine
        {
            std::map<std::string, std::string> tags;
            std::string prefix, command;
            std::vector<std::string> params;
            std::string trailing;
        };

        IrcLine Parse(const std::string& line)
        {
            IrcLine m;
            size_t pos = 0;
            if (!line.empty() && line[0] == '@')
            {
                size_t end = line.find(' ');
                if (end == std::string::npos) return m;
                for (auto& kv : Split(line.substr(1, end - 1), ';'))
                {
                    size_t eq = kv.find('=');
                    if (eq == std::string::npos) m.tags[kv] = "";
                    else m.tags[kv.substr(0, eq)] = UnescapeTag(kv.substr(eq + 1));
                }
                pos = end + 1;
            }
            if (pos < line.size() && line[pos] == ':')
            {
                size_t end = line.find(' ', pos);
                if (end == std::string::npos) return m;
                m.prefix = line.substr(pos + 1, end - pos - 1);
                pos = end + 1;
            }
            size_t trail = line.find(" :", pos);
            std::string middle = line.substr(pos, trail == std::string::npos ? std::string::npos : trail - pos);
            if (trail != std::string::npos) m.trailing = line.substr(trail + 2);
            auto parts = Split(middle, ' ');
            if (!parts.empty()) { m.command = parts[0]; parts.erase(parts.begin()); }
            m.params = parts;
            return m;
        }

        std::string Tag(const IrcLine& m, const char* key)
        {
            auto it = m.tags.find(key);
            return it == m.tags.end() ? "" : it->second;
        }

        int SubTierFromBadges(const std::string& badges)
        {
            // badges=subscriber/3012,... — tier is encoded in the badge version's thousands digit.
            for (auto& b : Split(badges, ','))
            {
                if (!StartsWith(b, "subscriber/") && !StartsWith(b, "founder/")) continue;
                int version = ToInt(b.substr(b.find('/') + 1), 0);
                if (version >= 3000) return 3;
                if (version >= 2000) return 2;
                return 1;
            }
            return 0;
        }

        int SubTierFromPlan(const std::string& plan)
        {
            if (plan == "3000") return 3;
            if (plan == "2000") return 2;
            return 1; // 1000 and Prime
        }
    }

    void TwitchChat::Start(std::string channel, std::string nick, std::string token)
    {
        m_channel = std::move(channel);
        m_nick = std::move(nick);
        m_token = std::move(token);
        m_running = true;
        m_thread = std::thread(&TwitchChat::Run, this);
        m_sender = std::thread(&TwitchChat::FlushOutbox, this);
    }

    void TwitchChat::Stop()
    {
        m_running = false;
        CloseHandles();
        if (m_thread.joinable()) m_thread.join();
        if (m_sender.joinable()) m_sender.join();
    }

    void TwitchChat::Say(const std::string& text)
    {
        if (!m_running) return; // Twitch isn't configured, so there's nowhere for this to go

        // A line break would end the PRIVMSG and let the rest run as an IRC command.
        std::string line = text;
        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == '\r' || c == '\n'; }), line.end());

        std::lock_guard lock(m_outboxMutex);
        if (m_outbox.size() < 50) m_outbox.push_back(std::move(line));
    }

    void TwitchChat::InjectLine(const std::string& line)
    {
        HandleLine(line, true);
    }

    // Twitch allows ~20 messages / 30s for normal accounts; stay well under it.
    void TwitchChat::FlushOutbox()
    {
        while (m_running)
        {
            Sleep(1600);
            if (!m_connected) continue;
            std::string next;
            {
                std::lock_guard lock(m_outboxMutex);
                if (m_outbox.empty()) continue;
                next = std::move(m_outbox.front());
                m_outbox.pop_front();
            }
            SendRaw("PRIVMSG #" + m_channel + " :" + next);
        }
    }

    void TwitchChat::CloseHandles()
    {
        std::lock_guard lock(m_sendMutex);
        m_connected = false;
        if (m_socket) { WinHttpWebSocketClose(m_socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); WinHttpCloseHandle(m_socket); m_socket = nullptr; }
        if (m_request) { WinHttpCloseHandle(m_request); m_request = nullptr; }
        if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
        if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
    }

    bool TwitchChat::SendRaw(const std::string& line)
    {
        std::lock_guard lock(m_sendMutex);
        if (!m_socket) return false;
        std::string data = line + "\r\n";
        DWORD err = WinHttpWebSocketSend(m_socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, data.data(), (DWORD)data.size());
        return err == ERROR_SUCCESS;
    }

    bool TwitchChat::ConnectOnce()
    {
        m_session = WinHttpOpen(L"ChatVsStreamerGTAV/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session) return false;
        m_connect = WinHttpConnect(m_session, L"irc-ws.chat.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!m_connect) return false;
        m_request = WinHttpOpenRequest(m_connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!m_request) return false;
        if (!WinHttpSetOption(m_request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
        if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) return false;
        if (!WinHttpReceiveResponse(m_request, nullptr)) return false;

        HINTERNET ws = WinHttpWebSocketCompleteUpgrade(m_request, 0);
        if (!ws) return false;
        {
            std::lock_guard lock(m_sendMutex);
            m_socket = ws;
        }
        WinHttpCloseHandle(m_request);
        m_request = nullptr;

        SendRaw("CAP REQ :twitch.tv/tags twitch.tv/commands");
        SendRaw("PASS " + m_token);
        SendRaw("NICK " + m_nick);
        SendRaw("JOIN #" + m_channel);
        return true;
    }

    void TwitchChat::Run()
    {
        int backoffMs = 2000;
        while (m_running)
        {
            if (!ConnectOnce())
            {
                log::Warn("Twitch connect failed (error {}), retrying in {}s", GetLastError(), backoffMs / 1000);
                CloseHandles();
                Sleep(backoffMs);
                backoffMs = std::min(backoffMs * 2, 60000);
                continue;
            }

            std::string pending;
            std::vector<char> buf(16384);
            while (m_running)
            {
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                DWORD err = WinHttpWebSocketReceive(m_socket, buf.data(), (DWORD)buf.size(), &read, &type);
                if (err != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;

                pending.append(buf.data(), read);
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;

                size_t start = 0, nl;
                while ((nl = pending.find("\r\n", start)) != std::string::npos)
                {
                    const std::string line = pending.substr(start, nl - start);
                    log::Guard("Handling a chat line", [&] { HandleLine(line, false); });
                    start = nl + 2;
                }
                pending.erase(0, start);
                backoffMs = 2000;
            }

            CloseHandles();
            if (m_running)
            {
                log::Warn("Twitch connection lost, reconnecting in {}s", backoffMs / 1000);
                Sleep(backoffMs);
                backoffMs = std::min(backoffMs * 2, 60000);
            }
        }
    }

    void TwitchChat::HandleLine(const std::string& line, bool simulated)
    {
        if (line.empty()) return;
        IrcLine m = Parse(line);

        if (!simulated)
        {
            if (m.command == "PING") { SendRaw("PONG :" + m.trailing); return; }
            if (m.command == "RECONNECT") { CloseHandles(); return; }
            if (m.command == "001") { log::Info("Logged in to Twitch as {}", m_nick); return; }
            if (m.command == "JOIN") { m_connected = true; log::Info("Joined #{}", m_channel); return; }
            if (m.command == "NOTICE")
            {
                log::Warn("Twitch notice: {}", m.trailing);
                if (m.trailing.find("Login authentication failed") != std::string::npos ||
                    m.trailing.find("Improperly formatted auth") != std::string::npos)
                    log::Error("Twitch rejected the OAuthToken in ChatVsStreamer.ini");
                return;
            }
        }

        std::string login = Lower(m.prefix.substr(0, m.prefix.find('!')));

        if (m.command == "PRIVMSG" && onMessage)
        {
            if (!IsValidLogin(login)) return;
            ChatMessage msg;
            msg.login = login;
            msg.display = Tag(m, "display-name");
            if (msg.display.empty()) msg.display = login;
            msg.text = m.trailing;
            msg.color = Tag(m, "color");
            msg.bits = ToInt(Tag(m, "bits"), 0);
            std::string badges = Tag(m, "badges");
            msg.subTier = SubTierFromBadges(badges);
            msg.isBroadcaster = badges.find("broadcaster/") != std::string::npos;
            msg.isMod = msg.isBroadcaster || Tag(m, "mod") == "1";
            msg.simulated = simulated;
            onMessage(msg);
        }
        else if (m.command == "USERNOTICE" && onEvent)
        {
            ChatEvent ev;
            ev.type = Tag(m, "msg-id");
            ev.login = Lower(Tag(m, "login"));
            if (!IsValidLogin(ev.login)) return;
            ev.display = Tag(m, "display-name");
            if (ev.display.empty()) ev.display = ev.login;
            ev.subTier = SubTierFromPlan(Tag(m, "msg-param-sub-plan"));
            if (ev.type == "raid") ev.count = ToInt(Tag(m, "msg-param-viewerCount"), 0);
            else if (ev.type == "submysterygift") ev.count = ToInt(Tag(m, "msg-param-mass-gift-count"), 1);
            else ev.count = 1;
            ev.simulated = simulated;
            onEvent(ev);
        }
    }
}
