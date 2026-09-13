using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using TwitchLib.Api;
using TwitchLib.Api.Core.Enums;

namespace ChaosMod.Core.Chat.Twitch
{
    public sealed class ChatterIdentity
    {
        public string UserId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
    }

    public sealed class ClipCreationResult
    {
        public bool Success { get; set; }
        public string EditUrl { get; set; }
        public string Error { get; set; }
    }

    /// <summary>
    /// Thin wrapper over TwitchLib.Api's Helix client, scoped to exactly what
    /// this mod needs: creating EventSub subscriptions against a websocket
    /// session, and creating clips. Uses the broadcaster's access token, since
    /// both operations require broadcaster-scoped permissions.
    /// </summary>
    public sealed class TwitchApiClient
    {
        private readonly TwitchAPI _api;

        public TwitchApiClient(string clientId, string broadcasterAccessToken)
        {
            _api = new TwitchAPI();
            _api.Settings.ClientId = clientId;
            _api.Settings.AccessToken = broadcasterAccessToken;
        }

        public void UpdateAccessToken(string accessToken) => _api.Settings.AccessToken = accessToken;

        public async Task CreateEventSubSubscriptionAsync(string type, string version, Dictionary<string, string> condition, string websocketSessionId)
        {
            await _api.Helix.EventSub.CreateEventSubSubscriptionAsync(
                type,
                version,
                condition,
                EventSubTransportMethod.Websocket,
                websocketSessionId: websocketSessionId).ConfigureAwait(false);
        }

        /// <summary>
        /// First page (up to 100) of the channel's current chatters, used only for
        /// the optional lurker point trickle. Requires moderator:read:chatters on
        /// the broadcaster token. Not paginated beyond the first page — fine for
        /// personal-stream-scale audiences; a very large channel would need to
        /// follow the response's cursor for a complete list.
        /// </summary>
        public async Task<List<ChatterIdentity>> GetChattersAsync(string broadcasterUserId, string moderatorUserId)
        {
            var response = await _api.Helix.Chat.GetChattersAsync(broadcasterUserId, moderatorUserId, 100).ConfigureAwait(false);
            var result = new List<ChatterIdentity>();
            if (response?.Data != null)
                foreach (var chatter in response.Data)
                    result.Add(new ChatterIdentity { UserId = chatter.UserId, Login = chatter.UserLogin, DisplayName = chatter.UserName });
            return result;
        }

        public async Task<ClipCreationResult> CreateClipAsync(string broadcasterUserId)
        {
            try
            {
                var response = await _api.Helix.Clips.CreateClipAsync(broadcasterUserId).ConfigureAwait(false);
                if (response?.CreatedClips == null || response.CreatedClips.Length == 0)
                    return new ClipCreationResult { Success = false, Error = "Twitch returned no clip (channel may not be live)." };

                var created = response.CreatedClips[0];
                return new ClipCreationResult { Success = true, EditUrl = created.EditUrl };
            }
            catch (Exception ex)
            {
                return new ClipCreationResult { Success = false, Error = ex.Message };
            }
        }
    }
}
