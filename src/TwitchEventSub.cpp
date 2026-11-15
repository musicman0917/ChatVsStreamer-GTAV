#include "TwitchEventSub.h"
#include "Json.h"
#include "Log.h"
#include "Util.h"

#include <algorithm>
#include <format>
#include <vector>

namespace cvs
{
    namespace
    {
        constexpr const char* kRedemptionType = "channel.channel_points_custom_reward_redemption.add";
        // Twitch sends a keepalive when nothing else has gone out for this long.
        constexpr int kKeepaliveSec = 30;

        // Real time, unaffected by the tests' time scale, for the connection watchdog.
        double Clock() { return GetTickCount64() / 1000.0; }

        struct HttpResult
        {
            DWORD status = 0; // 0 when no answer came back
            DWORD error = 0;
            std::string body;
        };

        // One HTTPS request through WinHTTP, so the plugin needs no networking library.
        HttpResult Https(const wchar_t* method, const wchar_t* host, const wchar_t* path,
                         const std::wstring& headers, const std::string& body)
        {
            HttpResult result;
            HINTERNET session = WinHttpOpen(L"ChatVsStreamerGTAV/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            HINTERNET connect = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
            HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
            if (request)
            {
                WinHttpSetTimeouts(request, 10000, 10000, 10000, 15000);
                const BOOL sent = WinHttpSendRequest(request,
                                                     headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                                     headers.empty() ? 0 : (DWORD)-1L,
                                                     body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                                     (DWORD)body.size(), (DWORD)body.size(), 0);
                if (sent && WinHttpReceiveResponse(request, nullptr))
                {
                    DWORD status = 0, size = sizeof(status);
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                    result.status = status;
                    DWORD available = 0;
                    while (WinHttpQueryDataAvailable(request, &available) && available > 0 && result.body.size() < (1u << 20))
                    {
                        std::vector<char> chunk(available);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                        result.body.append(chunk.data(), read);
                    }
                }
            }
            if (!result.status) result.error = GetLastError();
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            if (session) WinHttpCloseHandle(session);
            return result;
        }

        std::string Describe(const HttpResult& r)
        {
            if (!r.status) return std::format("no answer, error {}", r.error);
            // Twitch's error replies carry a readable "message".
            auto doc = Json::Parse(r.body);
            const std::string message = doc ? (*doc)["message"].Str() : "";
            return message.empty() ? std::format("HTTP {}", r.status) : std::format("HTTP {}: {}", r.status, message);
        }
    }

    void TwitchEventSub::Start(std::string channel, std::string token)
    {
        m_channel = std::move(channel);
        m_token = std::move(token);
        if (StartsWith(m_token, "oauth:")) m_token.erase(0, 6); // Helix wants the bare token
        m_running = true;
        m_thread = std::thread(&TwitchEventSub::Run, this);
        m_watchdog = std::thread(&TwitchEventSub::Watchdog, this);
    }

    void TwitchEventSub::Stop()
    {
        m_running = false;
        CloseHandles();
        if (m_thread.joinable()) m_thread.join();
        if (m_watchdog.joinable()) m_watchdog.join();
    }

    void TwitchEventSub::InjectMessage(const std::string& json)
    {
        HandleMessage(json, true);
    }

    void TwitchEventSub::CloseHandles()
    {
        std::lock_guard lock(m_handleMutex);
        if (m_socket) { WinHttpWebSocketClose(m_socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); WinHttpCloseHandle(m_socket); m_socket = nullptr; }
        if (m_request) { WinHttpCloseHandle(m_request); m_request = nullptr; }
        if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
        if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
    }

    // Asks Twitch who the token belongs to, which also gives the ids Helix needs.
    bool TwitchEventSub::CheckToken()
    {
        const HttpResult r = Https(L"GET", L"id.twitch.tv", L"/oauth2/validate", L"Authorization: OAuth " + Widen(m_token) + L"\r\n", "");
        if (r.status == 401)
        {
            log::Error("Twitch rejected BroadcasterToken in ChatVsStreamer.ini, so channel point rewards are off. "
                       "It may have expired: make a new one and restart the game.");
            m_giveUp = true;
            return false;
        }
        auto doc = r.status == 200 ? Json::Parse(r.body) : std::nullopt;
        if (!doc)
        {
            log::Warn("Couldn't check BroadcasterToken with Twitch ({}), retrying", Describe(r));
            return false;
        }

        const std::string login = Lower((*doc)["login"].Str());
        if (login != m_channel)
        {
            log::Error("BroadcasterToken in ChatVsStreamer.ini belongs to {}, but channel point rewards need a token for your channel, {}. "
                       "They're off until it's replaced.", login.empty() ? "an app rather than an account" : login, m_channel);
            m_giveUp = true;
            return false;
        }
        bool canRead = false;
        const Json& scopes = (*doc)["scopes"];
        for (size_t i = 0; i < scopes.Size(); ++i)
        {
            const std::string scope = scopes.At(i).Str();
            if (scope == "channel:read:redemptions" || scope == "channel:manage:redemptions") canRead = true;
        }
        if (!canRead)
        {
            log::Error("BroadcasterToken in ChatVsStreamer.ini doesn't have the channel:read:redemptions scope, so channel point "
                       "rewards are off. Make a token with that scope and restart the game.");
            m_giveUp = true;
            return false;
        }

        m_clientId = (*doc)["client_id"].Str();
        m_userId = (*doc)["user_id"].Str();
        if (m_clientId.empty() || m_userId.empty())
        {
            log::Warn("Twitch's token check left out the ids it should send, retrying");
            m_clientId.clear();
            return false;
        }
        const double expiresIn = (*doc)["expires_in"].Num(0);
        if (expiresIn > 0 && expiresIn < 3 * 86400)
            log::Warn("BroadcasterToken expires in about {} hours. Channel point rewards stop then, until you replace it.",
                      (int)(expiresIn / 3600) + 1);
        return true;
    }

    bool TwitchEventSub::ConnectOnce()
    {
        m_session = WinHttpOpen(L"ChatVsStreamerGTAV/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session) return false;
        m_connect = WinHttpConnect(m_session, L"eventsub.wss.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!m_connect) return false;
        const std::wstring path = L"/ws?keepalive_timeout_seconds=" + std::to_wstring(kKeepaliveSec);
        m_request = WinHttpOpenRequest(m_connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!m_request) return false;
        if (!WinHttpSetOption(m_request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
        if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) return false;
        if (!WinHttpReceiveResponse(m_request, nullptr)) return false;

        HINTERNET ws = WinHttpWebSocketCompleteUpgrade(m_request, 0);
        if (!ws) return false;
        {
            std::lock_guard lock(m_handleMutex);
            m_socket = ws;
        }
        WinHttpCloseHandle(m_request);
        m_request = nullptr;
        m_lastMessage = Clock();
        return true;
    }

    bool TwitchEventSub::Subscribe(const std::string& sessionId)
    {
        const std::string body = std::format(
            R"({{"type":"{}","version":"1","condition":{{"broadcaster_user_id":"{}"}},"transport":{{"method":"websocket","session_id":"{}"}}}})",
            kRedemptionType, JsonEscape(m_userId), JsonEscape(sessionId));
        const std::wstring headers = L"Authorization: Bearer " + Widen(m_token) + L"\r\nClient-Id: " + Widen(m_clientId) +
                                     L"\r\nContent-Type: application/json\r\n";
        const HttpResult r = Https(L"POST", L"api.twitch.tv", L"/helix/eventsub/subscriptions", headers, body);
        if (r.status == 202 || r.status == 409) return true; // 409: this connection already has it
        if (r.status == 401 || r.status == 403)
        {
            log::Error("Twitch won't send channel point redemptions ({}). Check BroadcasterToken in ChatVsStreamer.ini.", Describe(r));
            m_giveUp = true;
            return false;
        }
        log::Warn("Couldn't sign up for channel point redemptions ({}), retrying", Describe(r));
        return false;
    }

    void TwitchEventSub::Run()
    {
        int backoffMs = 2000;
        const auto pause = [this, &backoffMs]
        {
            for (int waited = 0; waited < backoffMs && m_running; waited += 200) Sleep(200);
            backoffMs = std::min(backoffMs * 2, 60000);
        };

        while (m_running && !m_giveUp)
        {
            if (m_clientId.empty() && !CheckToken())
            {
                if (!m_giveUp) pause();
                continue;
            }
            if (!ConnectOnce())
            {
                log::Warn("Couldn't connect to Twitch for channel point rewards (error {}), retrying in {}s", GetLastError(), backoffMs / 1000);
                CloseHandles();
                pause();
                continue;
            }

            HINTERNET socket;
            {
                std::lock_guard lock(m_handleMutex);
                socket = m_socket;
            }
            std::string message;
            std::vector<char> buf(16384);
            while (m_running && socket)
            {
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                if (WinHttpWebSocketReceive(socket, buf.data(), (DWORD)buf.size(), &read, &type) != ERROR_SUCCESS) break;
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                message.append(buf.data(), read);
                if (message.size() > (1u << 20)) break; // nothing Twitch sends here is anywhere near this big
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
                log::Guard("Handling a channel point message", [&] { HandleMessage(message, false); });
                message.clear();
                if (m_listening) backoffMs = 2000;
                if (m_reconnectNow || m_giveUp) break;
            }

            m_listening = false;
            CloseHandles();
            if (!m_running || m_giveUp) break;
            if (m_reconnectNow.exchange(false)) continue; // Twitch asked for this, so go straight away
            log::Warn("Lost the channel point connection to Twitch, reconnecting in {}s", backoffMs / 1000);
            pause();
        }
        m_listening = false;
    }

    // Twitch promises a message at least every keepalive period, so a longer silence means
    // the connection died without saying so. Closing it makes Run() reconnect.
    void TwitchEventSub::Watchdog()
    {
        while (m_running && !m_giveUp)
        {
            Sleep(500);
            bool open;
            {
                std::lock_guard lock(m_handleMutex);
                open = m_socket != nullptr;
            }
            if (open && Clock() - m_lastMessage.load() > m_keepaliveSec.load() + 10)
            {
                log::Warn("Twitch's channel point connection went quiet, reconnecting");
                m_lastMessage = Clock();
                CloseHandles();
            }
        }
    }

    void TwitchEventSub::HandleMessage(const std::string& json, bool simulated)
    {
        if (!simulated) m_lastMessage = Clock();
        auto doc = Json::Parse(json);
        if (!doc) return;
        const Json& metadata = (*doc)["metadata"];
        const std::string type = metadata["message_type"].Str();

        if (type == "notification")
        {
            if (metadata["subscription_type"].Str() != kRedemptionType) return;
            const Json& ev = (*doc)["payload"]["event"];
            Redemption r;
            r.id = ev["id"].Str();
            r.login = Lower(ev["user_login"].Str());
            r.display = ev["user_name"].Str();
            r.rewardTitle = ev["reward"]["title"].Str();
            r.cost = (int)ev["reward"]["cost"].Num(0);
            r.simulated = simulated;
            if (!IsValidLogin(r.login) || r.rewardTitle.empty()) return;
            if (r.display.empty()) r.display = r.login;
            // Twitch can deliver the same message twice.
            if (!FirstTime(r.id.empty() ? metadata["message_id"].Str() : r.id)) return;
            if (onRedemption) onRedemption(r);
            return;
        }
        if (simulated) return; // the connection itself is never simulated

        if (type == "session_welcome")
        {
            const Json& session = (*doc)["payload"]["session"];
            m_keepaliveSec = std::clamp((int)session["keepalive_timeout_seconds"].Num(kKeepaliveSec), 10, 600);
            // Twitch drops the connection unless something is subscribed within 10 seconds.
            if (Subscribe(session["id"].Str()))
            {
                m_listening = true;
                log::Info("Listening for channel point rewards on #{}", m_channel);
            }
            else if (!m_giveUp) CloseHandles(); // try again on a fresh connection
        }
        else if (type == "session_reconnect")
        {
            log::Info("Twitch is moving the channel point connection, reconnecting");
            m_reconnectNow = true;
        }
        else if (type == "revocation")
        {
            log::Error("Twitch stopped sending channel point redemptions ({}). Check BroadcasterToken in ChatVsStreamer.ini.",
                       (*doc)["payload"]["subscription"]["status"].Str("no reason given"));
            m_giveUp = true;
        }
        // session_keepalive only shows the connection is alive, which m_lastMessage already covers.
    }

    bool TwitchEventSub::FirstTime(const std::string& id)
    {
        if (id.empty()) return true;
        std::lock_guard lock(m_seenMutex);
        if (!m_seen.insert(id).second) return false;
        m_seenOrder.push_back(id);
        if (m_seenOrder.size() > 500)
        {
            m_seen.erase(m_seenOrder.front());
            m_seenOrder.pop_front();
        }
        return true;
    }
}
