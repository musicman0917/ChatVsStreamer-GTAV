using System.Net;
using System.Threading.Tasks;

namespace ChaosMod.Core.Net
{
    /// <summary>
    /// .NET Framework 4.8's HttpListener has no native GetContextAsync (that was
    /// only added in later .NET runtimes) — this wraps the Begin/End pair so both
    /// OverlayServer and TwitchOAuthService can await it like a normal async API.
    /// </summary>
    public static class HttpListenerExtensions
    {
        public static Task<HttpListenerContext> GetContextAsync(this HttpListener listener)
        {
            return Task.Factory.FromAsync(listener.BeginGetContext, listener.EndGetContext, null);
        }
    }
}
