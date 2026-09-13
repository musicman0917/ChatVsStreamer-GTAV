using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Net;

namespace ChaosMod.Core.Overlay
{
    public sealed class OverlayOptions
    {
        public bool Enabled { get; set; } = true;
        public int Port { get; set; } = 8420;
        public bool BindLoopbackOnly { get; set; } = true;
        public string StaticRootPath { get; set; }
    }

    /// <summary>
    /// Local HttpListener serving the OBS browser-source overlay's static
    /// assets plus a raw WebSocket endpoint (/ws) that broadcasts JSON events.
    /// Deliberately not ASP.NET/Kestrel — this runs inside the GTA V process
    /// via SHVDN, where a full web host is unnecessary weight.
    /// </summary>
    public sealed class OverlayServer : IDisposable
    {
        private readonly OverlayOptions _options;
        private readonly JsonSerializerOptions _jsonOptions = new JsonSerializerOptions();

        private HttpListener _listener;
        private CancellationTokenSource _cts;
        private Task _acceptLoop;

        private readonly ConcurrentDictionary<Guid, ManagedSocket> _sockets = new ConcurrentDictionary<Guid, ManagedSocket>();

        public Func<SnapshotDto> BuildSnapshot { get; set; }

        public int ConnectedClients => _sockets.Count;

        public OverlayServer(OverlayOptions options)
        {
            _options = options;
        }

        public Task StartAsync()
        {
            if (!_options.Enabled) return Task.CompletedTask;

            var host = _options.BindLoopbackOnly ? "localhost" : "+";
            _listener = new HttpListener();
            _listener.Prefixes.Add($"http://{host}:{_options.Port}/");

            try
            {
                _listener.Start();
            }
            catch (HttpListenerException ex)
            {
                throw new InvalidOperationException(
                    $"Could not bind http://{host}:{_options.Port}/ ({ex.Message}). " +
                    $"On Windows this usually needs a URL ACL reservation: run once, elevated, " +
                    $"'netsh http add urlacl url=http://{host}:{_options.Port}/ user=Everyone'. See docs/SETUP.md.", ex);
            }

            _cts = new CancellationTokenSource();
            _acceptLoop = Task.Run(() => AcceptLoopAsync(_cts.Token));
            return Task.CompletedTask;
        }

        public Task StopAsync()
        {
            try { _cts?.Cancel(); } catch { /* ignore */ }
            try { _listener?.Stop(); } catch { /* ignore */ }
            return Task.CompletedTask;
        }

        private async Task AcceptLoopAsync(CancellationToken ct)
        {
            while (!ct.IsCancellationRequested && _listener.IsListening)
            {
                HttpListenerContext context;
                try
                {
                    context = await _listener.GetContextAsync().ConfigureAwait(false);
                }
                catch (Exception)
                {
                    if (ct.IsCancellationRequested) return;
                    continue;
                }

                _ = HandleContextAsync(context, ct);
            }
        }

        private async Task HandleContextAsync(HttpListenerContext context, CancellationToken ct)
        {
            try
            {
                if (context.Request.Url.AbsolutePath == "/ws")
                {
                    if (!context.Request.IsWebSocketRequest)
                    {
                        context.Response.StatusCode = 400;
                        context.Response.Close();
                        return;
                    }

                    var wsContext = await context.AcceptWebSocketAsync(null).ConfigureAwait(false);
                    var socket = new ManagedSocket(wsContext.WebSocket);
                    var id = Guid.NewGuid();
                    _sockets[id] = socket;

                    var snapshot = BuildSnapshot?.Invoke();
                    if (snapshot != null)
                        await socket.SendAsync(JsonSerializer.Serialize(snapshot, _jsonOptions)).ConfigureAwait(false);

                    await PumpReceiveLoopAsync(id, socket, ct).ConfigureAwait(false);
                    return;
                }

                await ServeStaticFileAsync(context).ConfigureAwait(false);
            }
            catch (Exception)
            {
                try { context.Response.StatusCode = 500; context.Response.Close(); } catch { /* ignore */ }
            }
        }

        private async Task PumpReceiveLoopAsync(Guid id, ManagedSocket socket, CancellationToken ct)
        {
            var buffer = new byte[1024];
            try
            {
                while (socket.Socket.State == WebSocketState.Open && !ct.IsCancellationRequested)
                {
                    var result = await socket.Socket.ReceiveAsync(new ArraySegment<byte>(buffer), ct).ConfigureAwait(false);
                    if (result.MessageType == WebSocketMessageType.Close) break;
                }
            }
            catch (Exception) { /* client disconnected */ }
            finally
            {
                _sockets.TryRemove(id, out _);
                try { await socket.Socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None).ConfigureAwait(false); } catch { /* ignore */ }
            }
        }

        private async Task ServeStaticFileAsync(HttpListenerContext context)
        {
            var relative = context.Request.Url.AbsolutePath.TrimStart('/');
            if (string.IsNullOrEmpty(relative)) relative = "index.html";

            var fullPath = Path.GetFullPath(Path.Combine(_options.StaticRootPath, relative));
            if (!fullPath.StartsWith(Path.GetFullPath(_options.StaticRootPath), StringComparison.OrdinalIgnoreCase) || !File.Exists(fullPath))
            {
                context.Response.StatusCode = 404;
                context.Response.Close();
                return;
            }

            context.Response.ContentType = GuessContentType(fullPath);
            var bytes = await Task.Run(() => File.ReadAllBytes(fullPath)).ConfigureAwait(false);
            context.Response.ContentLength64 = bytes.Length;
            await context.Response.OutputStream.WriteAsync(bytes, 0, bytes.Length).ConfigureAwait(false);
            context.Response.Close();
        }

        private static string GuessContentType(string path)
        {
            if (path.EndsWith(".html")) return "text/html; charset=utf-8";
            if (path.EndsWith(".js")) return "application/javascript; charset=utf-8";
            if (path.EndsWith(".css")) return "text/css; charset=utf-8";
            if (path.EndsWith(".json")) return "application/json";
            if (path.EndsWith(".png")) return "image/png";
            if (path.EndsWith(".svg")) return "image/svg+xml";
            return "application/octet-stream";
        }

        public void Broadcast<T>(T evt)
        {
            if (_sockets.IsEmpty) return;
            var json = JsonSerializer.Serialize(evt, _jsonOptions);
            foreach (var socket in _sockets.Values)
                _ = socket.SendAsync(json);
        }

        public void Dispose()
        {
            StopAsync().Wait(TimeSpan.FromSeconds(2));
            foreach (var s in _sockets.Values) s.Dispose();
            _listener?.Close();
        }

        private sealed class ManagedSocket : IDisposable
        {
            public WebSocket Socket { get; }
            private readonly SemaphoreSlim _sendGate = new SemaphoreSlim(1, 1);

            public ManagedSocket(WebSocket socket) => Socket = socket;

            public async Task SendAsync(string json)
            {
                var bytes = Encoding.UTF8.GetBytes(json);
                await _sendGate.WaitAsync().ConfigureAwait(false);
                try
                {
                    if (Socket.State == WebSocketState.Open)
                        await Socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, CancellationToken.None).ConfigureAwait(false);
                }
                catch { /* client likely disconnected mid-send */ }
                finally { _sendGate.Release(); }
            }

            public void Dispose() => _sendGate.Dispose();
        }
    }
}
