using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Net;

namespace ChaosMod.Core.Chat.Twitch
{
    public sealed class TwitchOAuthOptions
    {
        public string ClientId { get; set; }
        public int RedirectPort { get; set; } = 3800;

        public static readonly string[] BotScopes = { "chat:read", "chat:edit" };
        public static readonly string[] BroadcasterScopes =
        {
            "bits:read", "channel:read:subscriptions", "moderator:read:followers",
            "channel:read:redemptions", "channel:manage:redemptions", "clips:edit",
            // Only needed if [Points] EnableLurkerTrickle=true; requested unconditionally
            // since scopes can't be added later without re-authorizing.
            "moderator:read:chatters"
        };
    }

    public sealed class OAuthResult
    {
        public bool Success { get; set; }
        public string Error { get; set; }
        public TwitchTokenRecord Token { get; set; }
    }

    /// <summary>
    /// Drives Twitch's Implicit Grant flow entirely in-mod: opens the user's
    /// browser to Twitch's authorize endpoint, and runs a tiny local HttpListener
    /// that captures the resulting access token via a client-side redirect page
    /// (Twitch returns implicit-grant tokens in the URL fragment, which browsers
    /// never send to a server — the landing page's own JS forwards it back as a
    /// query string on a second request). See docs/SETUP.md for the one manual
    /// step this can't automate: registering a Twitch Developer application.
    /// </summary>
    public sealed class TwitchOAuthService : IDisposable
    {
        private readonly TwitchOAuthOptions _options;
        private readonly TwitchTokenStore _tokenStore;
        private readonly HttpClient _http = new HttpClient();

        private HttpListener _listener;
        private CancellationTokenSource _listenerCts;
        private Task _listenerLoop;

        private readonly ConcurrentDictionary<string, TaskCompletionSource<OAuthResult>> _pending =
            new ConcurrentDictionary<string, TaskCompletionSource<OAuthResult>>();

        public event EventHandler<string> StatusChanged;

        public TwitchOAuthService(TwitchOAuthOptions options, TwitchTokenStore tokenStore)
        {
            _options = options;
            _tokenStore = tokenStore;
        }

        public Task<OAuthResult> AuthorizeBotAccountAsync(CancellationToken ct) =>
            AuthorizeAsync(TwitchTokenRole.Bot, TwitchOAuthOptions.BotScopes, ct);

        public Task<OAuthResult> AuthorizeBroadcasterAccountAsync(CancellationToken ct) =>
            AuthorizeAsync(TwitchTokenRole.Broadcaster, TwitchOAuthOptions.BroadcasterScopes, ct);

        private async Task<OAuthResult> AuthorizeAsync(TwitchTokenRole role, string[] scopes, CancellationToken ct)
        {
            if (string.IsNullOrWhiteSpace(_options.ClientId))
                return new OAuthResult { Success = false, Error = "ClientId is not configured. Set [Twitch] ClientId in config.ini first." };

            EnsureListenerRunning();

            var state = $"{role}:{Guid.NewGuid():N}";
            var tcs = new TaskCompletionSource<OAuthResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            _pending[state] = tcs;

            var redirectUri = $"http://localhost:{_options.RedirectPort}/callback";
            var scopeParam = Uri.EscapeDataString(string.Join(" ", scopes));
            var url = "https://id.twitch.tv/oauth2/authorize" +
                       $"?client_id={Uri.EscapeDataString(_options.ClientId)}" +
                       $"&redirect_uri={Uri.EscapeDataString(redirectUri)}" +
                       "&response_type=token" +
                       $"&scope={scopeParam}" +
                       $"&state={state}" +
                       "&force_verify=true";

            RaiseStatus($"Opening browser for {role} authorization…");
            try
            {
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                _pending.TryRemove(state, out _);
                return new OAuthResult { Success = false, Error = $"Could not open browser: {ex.Message}. Visit this URL manually: {url}" };
            }

            using (ct.Register(() => tcs.TrySetResult(new OAuthResult { Success = false, Error = "Cancelled." })))
            {
                var timeoutTask = Task.Delay(TimeSpan.FromMinutes(2), ct);
                var completed = await Task.WhenAny(tcs.Task, timeoutTask).ConfigureAwait(false);
                _pending.TryRemove(state, out _);

                if (completed == timeoutTask)
                {
                    RaiseStatus($"{role} authorization timed out.");
                    return new OAuthResult { Success = false, Error = "Timed out waiting for browser authorization." };
                }

                var result = await tcs.Task.ConfigureAwait(false);
                RaiseStatus(result.Success
                    ? $"{role} authorized as {result.Token.Login}."
                    : $"{role} authorization failed: {result.Error}");
                return result;
            }
        }

        private void EnsureListenerRunning()
        {
            if (_listener != null && _listener.IsListening) return;

            _listener = new HttpListener();
            _listener.Prefixes.Add($"http://localhost:{_options.RedirectPort}/");
            try
            {
                _listener.Start();
            }
            catch (HttpListenerException ex)
            {
                throw new InvalidOperationException(
                    $"Could not bind http://localhost:{_options.RedirectPort}/ ({ex.Message}). " +
                    "On Windows this usually means a URL ACL reservation is needed: run once, elevated, " +
                    $"'netsh http add urlacl url=http://localhost:{_options.RedirectPort}/ user=Everyone'. See docs/SETUP.md.", ex);
            }

            _listenerCts = new CancellationTokenSource();
            _listenerLoop = Task.Run(() => ListenLoopAsync(_listenerCts.Token));
        }

        private async Task ListenLoopAsync(CancellationToken ct)
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

                _ = HandleRequestAsync(context);
            }
        }

        private async Task HandleRequestAsync(HttpListenerContext context)
        {
            try
            {
                var path = context.Request.Url.AbsolutePath;

                if (string.Equals(path, "/callback", StringComparison.OrdinalIgnoreCase))
                {
                    await WriteHtmlAsync(context, CallbackPageHtml).ConfigureAwait(false);
                }
                else if (string.Equals(path, "/callback/token", StringComparison.OrdinalIgnoreCase))
                {
                    await HandleTokenCallbackAsync(context).ConfigureAwait(false);
                }
                else if (string.Equals(path, "/callback/error", StringComparison.OrdinalIgnoreCase))
                {
                    var state = context.Request.QueryString["state"];
                    var error = context.Request.QueryString["error"] ?? "access_denied";
                    CompletePending(state, new OAuthResult { Success = false, Error = error });
                    await WriteJsonAsync(context, "{\"ok\":true}").ConfigureAwait(false);
                }
                else
                {
                    context.Response.StatusCode = 404;
                    context.Response.Close();
                }
            }
            catch (Exception)
            {
                try { context.Response.StatusCode = 500; context.Response.Close(); } catch { /* ignore */ }
            }
        }

        private async Task HandleTokenCallbackAsync(HttpListenerContext context)
        {
            var query = context.Request.QueryString;
            var accessToken = query["access_token"];
            var state = query["state"];

            if (string.IsNullOrEmpty(accessToken) || string.IsNullOrEmpty(state))
            {
                await WriteJsonAsync(context, "{\"ok\":false,\"error\":\"missing token or state\"}").ConfigureAwait(false);
                return;
            }

            try
            {
                var validation = await ValidateTokenAsync(accessToken).ConfigureAwait(false);
                var role = state.Split(':')[0];

                var record = new TwitchTokenRecord
                {
                    Role = role,
                    AccessToken = accessToken,
                    Login = validation.Login,
                    UserId = validation.UserId,
                    Scopes = validation.Scopes,
                    ObtainedAtUtc = DateTime.UtcNow
                };
                _tokenStore.Save(record);

                CompletePending(state, new OAuthResult { Success = true, Token = record });
                await WriteJsonAsync(context, $"{{\"ok\":true,\"login\":\"{validation.Login}\"}}").ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                CompletePending(state, new OAuthResult { Success = false, Error = ex.Message });
                await WriteJsonAsync(context, $"{{\"ok\":false,\"error\":{JsonSerializer.Serialize(ex.Message)}}}").ConfigureAwait(false);
            }
        }

        private sealed class TokenValidation
        {
            public string Login;
            public string UserId;
            public System.Collections.Generic.List<string> Scopes;
        }

        private async Task<TokenValidation> ValidateTokenAsync(string accessToken)
        {
            using (var request = new HttpRequestMessage(HttpMethod.Get, "https://id.twitch.tv/oauth2/validate"))
            {
                request.Headers.Add("Authorization", $"OAuth {accessToken}");
                var response = await _http.SendAsync(request).ConfigureAwait(false);
                if (!response.IsSuccessStatusCode)
                    throw new InvalidOperationException($"Token validation failed ({(int)response.StatusCode}).");

                var json = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                using (var doc = JsonDocument.Parse(json))
                {
                    var root = doc.RootElement;
                    var scopes = new System.Collections.Generic.List<string>();
                    if (root.TryGetProperty("scopes", out var scopesEl))
                        foreach (var s in scopesEl.EnumerateArray()) scopes.Add(s.GetString());

                    return new TokenValidation
                    {
                        Login = root.GetProperty("login").GetString(),
                        UserId = root.GetProperty("user_id").GetString(),
                        Scopes = scopes
                    };
                }
            }
        }

        private void CompletePending(string state, OAuthResult result)
        {
            if (state != null && _pending.TryRemove(state, out var tcs))
                tcs.TrySetResult(result);
        }

        private static async Task WriteHtmlAsync(HttpListenerContext context, string html)
        {
            var bytes = Encoding.UTF8.GetBytes(html);
            context.Response.ContentType = "text/html; charset=utf-8";
            context.Response.ContentLength64 = bytes.Length;
            await context.Response.OutputStream.WriteAsync(bytes, 0, bytes.Length).ConfigureAwait(false);
            context.Response.Close();
        }

        private static async Task WriteJsonAsync(HttpListenerContext context, string json)
        {
            var bytes = Encoding.UTF8.GetBytes(json);
            context.Response.ContentType = "application/json";
            context.Response.ContentLength64 = bytes.Length;
            await context.Response.OutputStream.WriteAsync(bytes, 0, bytes.Length).ConfigureAwait(false);
            context.Response.Close();
        }

        private const string CallbackPageHtml = @"<!doctype html>
<html><head><meta charset=""utf-8""><title>Chaos Mod - Twitch Authorization</title>
<style>body{background:#0e0e12;color:#eee;font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0}
.card{background:#1b1b22;padding:2rem 3rem;border-radius:12px;text-align:center;box-shadow:0 0 40px rgba(160,60,255,.3)}
h1{color:#a63cff}</style></head>
<body><div class=""card""><h1 id=""title"">Working…</h1><p id=""msg"">Completing Twitch authorization.</p></div>
<script>
(function () {
  var hash = window.location.hash.substring(1);
  var params = new URLSearchParams(hash);
  var title = document.getElementById('title');
  var msg = document.getElementById('msg');
  if (params.get('error')) {
    fetch('/callback/error?state=' + encodeURIComponent(params.get('state') || '') + '&error=' + encodeURIComponent(params.get('error')))
      .then(function () { title.textContent = 'Authorization cancelled'; msg.textContent = 'You can close this tab.'; });
    return;
  }
  var token = params.get('access_token');
  var state = params.get('state');
  if (!token || !state) {
    title.textContent = 'Something went wrong';
    msg.textContent = 'No access token was found in the redirect. You can close this tab and try again.';
    return;
  }
  fetch('/callback/token?access_token=' + encodeURIComponent(token) + '&state=' + encodeURIComponent(state))
    .then(function (r) { return r.json(); })
    .then(function (data) {
      if (data.ok) { title.textContent = 'Authorized as ' + data.login; msg.textContent = 'You can close this tab and return to GTA V.'; }
      else { title.textContent = 'Authorization failed'; msg.textContent = data.error || 'Unknown error.'; }
    })
    .catch(function () { title.textContent = 'Authorization failed'; msg.textContent = 'Could not reach the local mod server.'; });
})();
</script>
</body></html>";

        public void Dispose()
        {
            try { _listenerCts?.Cancel(); } catch { /* ignore */ }
            try { _listener?.Stop(); _listener?.Close(); } catch { /* ignore */ }
            _http.Dispose();
        }
    }
}
