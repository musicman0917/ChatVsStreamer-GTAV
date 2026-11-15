#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <WinSock2.h>

namespace cvs
{
    struct HttpRequest
    {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers; // lowercase names
        std::string body;
    };

    struct HttpReply
    {
        int status = 200;
        std::string contentType = "application/json";
        std::string body = "{}";
    };

    // Tiny localhost-only HTTP server.
    //   GET  /            overlay.html (OBS browser source)
    //   GET  /panel       panel.html (streamer control panel)
    //   GET  /events      Server-Sent Events stream of toasts
    //   anything else     passed to onRequest
    //
    // Web pages the streamer happens to have open can reach localhost too, so every
    // request has its Host checked (blocks DNS rebinding) and every POST must also
    // carry X-CVS-Panel from a same-origin page (blocks cross-site requests).
    class WebServer
    {
    public:
        std::function<HttpReply(const HttpRequest&)> onRequest;

        bool Start(int port);
        void Stop();
        // Queues one SSE event (a JSON object). Never does network I/O on the
        // caller's thread, so it's safe to call from anywhere.
        void Broadcast(const std::string& json);

    private:
        void AcceptLoop();
        void BroadcastLoop();
        void HandleClient(SOCKET client);
        bool ReadRequest(SOCKET client, HttpRequest& out);
        bool IsAllowed(const HttpRequest& req) const;

        int m_port = 0;
        SOCKET m_listen = INVALID_SOCKET;
        std::thread m_acceptThread, m_broadcastThread;
        std::atomic<bool> m_running{ false };

        std::mutex m_clientsMutex;
        std::vector<SOCKET> m_sseClients;

        std::mutex m_outMutex;
        std::condition_variable m_outCv;
        std::deque<std::string> m_out;
    };
}
