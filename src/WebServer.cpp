#include "WebServer.h"
#include "Log.h"
#include "Util.h"

#include <fstream>
#include <sstream>

#include <WS2tcpip.h>

namespace cvs
{
    namespace
    {
        constexpr size_t kMaxHeaderBytes = 16 * 1024;
        constexpr size_t kMaxBodyBytes = 16 * 1024;
        constexpr size_t kMaxSseClients = 16;
        constexpr size_t kMaxQueuedEvents = 200;

        void SendAll(SOCKET s, const std::string& data)
        {
            const char* p = data.data();
            int left = (int)data.size();
            while (left > 0)
            {
                int n = send(s, p, left, 0);
                if (n <= 0) return;
                p += n;
                left -= n;
            }
        }

        const char* StatusText(int status)
        {
            switch (status)
            {
            case 200: return "200 OK";
            case 400: return "400 Bad Request";
            case 403: return "403 Forbidden";
            case 404: return "404 Not Found";
            default: return "500 Internal Server Error";
            }
        }

        void Respond(SOCKET s, const HttpReply& r)
        {
            SendAll(s, std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                   StatusText(r.status), r.contentType, r.body.size()) + r.body);
        }

        HttpReply ServeFile(const wchar_t* name)
        {
            std::ifstream in(DataDir() / name, std::ios::binary);
            if (!in) return { 404, "text/plain", Narrow(name) + " is missing from the mod's data folder. Re-run install.ps1." };
            std::stringstream ss;
            ss << in.rdbuf();
            return { 200, "text/html; charset=utf-8", ss.str() };
        }
    }

    bool WebServer::Start(int port)
    {
        m_port = port;
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(m_listen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(m_listen, 16) != 0)
        {
            log::Error("Web server could not bind 127.0.0.1:{} (in use?)", port);
            closesocket(m_listen);
            m_listen = INVALID_SOCKET;
            return false;
        }
        m_running = true;
        m_acceptThread = std::thread(&WebServer::AcceptLoop, this);
        m_broadcastThread = std::thread(&WebServer::BroadcastLoop, this);
        log::Info("Overlay at http://localhost:{}/  Control panel at http://localhost:{}/panel", port, port);
        return true;
    }

    void WebServer::Stop()
    {
        m_running = false;
        m_outCv.notify_all();
        if (m_listen != INVALID_SOCKET) { closesocket(m_listen); m_listen = INVALID_SOCKET; }
        if (m_acceptThread.joinable()) m_acceptThread.join();
        if (m_broadcastThread.joinable()) m_broadcastThread.join();
        std::lock_guard lock(m_clientsMutex);
        for (SOCKET c : m_sseClients) closesocket(c);
        m_sseClients.clear();
    }

    void WebServer::Broadcast(const std::string& json)
    {
        {
            std::lock_guard lock(m_outMutex);
            if (m_out.size() >= kMaxQueuedEvents) m_out.pop_front();
            m_out.push_back("data: " + json + "\n\n");
        }
        m_outCv.notify_one();
    }

    void WebServer::BroadcastLoop()
    {
        while (m_running)
        {
            std::string frame;
            {
                std::unique_lock lock(m_outMutex);
                m_outCv.wait(lock, [this] { return !m_running || !m_out.empty(); });
                if (!m_running) return;
                frame = std::move(m_out.front());
                m_out.pop_front();
            }
            std::lock_guard lock(m_clientsMutex);
            for (auto it = m_sseClients.begin(); it != m_sseClients.end();)
            {
                if (send(*it, frame.data(), (int)frame.size(), 0) <= 0)
                {
                    closesocket(*it);
                    it = m_sseClients.erase(it);
                }
                else ++it;
            }
        }
    }

    void WebServer::AcceptLoop()
    {
        while (m_running)
        {
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (m_running) Sleep(50);
                continue;
            }
            std::thread([this, client] { log::Guard("Handling a web request", [&] { HandleClient(client); }); }).detach();
        }
    }

    bool WebServer::ReadRequest(SOCKET client, HttpRequest& out)
    {
        std::string data;
        char buf[4096];
        size_t headerEnd;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos)
        {
            if (data.size() > kMaxHeaderBytes) return false;
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            data.append(buf, n);
        }

        auto lines = Split(data.substr(0, headerEnd), '\n');
        auto first = Split(Trim(lines[0]), ' ');
        if (first.size() < 2) return false;
        out.method = first[0];
        out.path = first[1].substr(0, first[1].find('?'));
        for (size_t i = 1; i < lines.size(); ++i)
        {
            size_t colon = lines[i].find(':');
            if (colon == std::string::npos) continue;
            out.headers[Lower(Trim(lines[i].substr(0, colon)))] = Trim(lines[i].substr(colon + 1));
        }

        size_t length = 0;
        if (auto it = out.headers.find("content-length"); it != out.headers.end())
        {
            int parsed = ToInt(it->second, -1);
            if (parsed < 0 || (size_t)parsed > kMaxBodyBytes) return false;
            length = (size_t)parsed;
        }
        out.body = data.substr(headerEnd + 4);
        while (out.body.size() < length)
        {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            out.body.append(buf, n);
        }
        out.body.resize(length);
        return true;
    }

    bool WebServer::IsAllowed(const HttpRequest& req) const
    {
        const std::string hostA = std::format("localhost:{}", m_port);
        const std::string hostB = std::format("127.0.0.1:{}", m_port);

        auto host = req.headers.find("host");
        if (host == req.headers.end()) return false;
        std::string h = Lower(host->second);
        if (h != hostA && h != hostB) return false;

        if (req.method == "GET") return true;
        if (req.method != "POST") return false;

        // A custom header can't be sent cross-origin without a CORS preflight, and
        // this server never approves one.
        if (req.headers.find("x-cvs-panel") == req.headers.end()) return false;
        auto origin = req.headers.find("origin");
        if (origin != req.headers.end())
        {
            std::string o = Lower(origin->second);
            if (o != "http://" + hostA && o != "http://" + hostB) return false;
        }
        return true;
    }

    void WebServer::HandleClient(SOCKET client)
    {
        DWORD timeoutMs = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

        HttpRequest req;
        if (!ReadRequest(client, req))
        {
            Respond(client, { 400, "text/plain", "bad request" });
            closesocket(client);
            return;
        }
        if (!IsAllowed(req))
        {
            log::Warn("Blocked {} {} (failed the local-origin check)", req.method, req.path);
            Respond(client, { 403, "text/plain", "forbidden" });
            closesocket(client);
            return;
        }

        if (req.method == "GET" && req.path == "/events")
        {
            std::lock_guard lock(m_clientsMutex);
            if (m_sseClients.size() >= kMaxSseClients)
            {
                Respond(client, { 400, "text/plain", "too many overlay connections" });
                closesocket(client);
                return;
            }
            SendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                            "Connection: keep-alive\r\n\r\n: connected\n\n");
            m_sseClients.push_back(client);
            return; // socket stays open; BroadcastLoop owns it now
        }

        HttpReply reply;
        if (req.method == "GET" && (req.path == "/" || req.path == "/overlay")) reply = ServeFile(L"overlay.html");
        else if (req.method == "GET" && req.path == "/panel") reply = ServeFile(L"panel.html");
        else if (onRequest)
        {
            try { reply = onRequest(req); }
            catch (const std::exception& e)
            {
                log::Error("Error handling {}: {}", req.path, e.what());
                reply = { 500, "text/plain", "internal error" };
            }
        }
        else reply = { 404, "text/plain", "not found" };

        Respond(client, reply);
        closesocket(client);
    }
}
