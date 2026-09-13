using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using Microsoft.Extensions.DependencyInjection;
using TwitchLib.EventSub.Websockets;
using TwitchLib.EventSub.Websockets.Core.EventArgs;
using TwitchLib.EventSub.Websockets.Core.EventArgs.Channel;

namespace ChaosMod.Core.Chat.Twitch
{
    /// <summary>
    /// IChannelEventSource over TwitchLib.EventSub.Websockets — subs, bits,
    /// follows, raids, and channel-point redemptions arrive here, since Twitch's
    /// PubSub API (the old way to get these) was fully decommissioned in 2025.
    ///
    /// NOTE: TwitchLib.EventSub.Websockets is still labeled "open beta" upstream
    /// and its event-arg shapes (the Notification.Payload.Event nesting below)
    /// have changed between package versions. If this doesn't compile cleanly
    /// against whatever version NuGet restores, check that package's own
    /// samples/IntelliSense for the current event-arg property paths and adjust
    /// accordingly — the subscription types/versions/conditions themselves
    /// (in ConnectAsync's CreateAllSubscriptions) are stable Twitch API surface
    /// and shouldn't need to change.
    /// </summary>
    public sealed class TwitchEventSubSource : IChannelEventSource, IDisposable
    {
        private readonly TwitchApiClient _api;
        private readonly string _broadcasterUserId;
        private EventSubWebsocketClient _client;
        private ServiceProvider _serviceProvider;

        public string SourceName => "Twitch";
        public bool IsConnected { get; private set; }

        public event EventHandler<SubscriptionEventArgs> Subscribed;
        public event EventHandler<BitsEventArgs> BitsCheered;
        public event EventHandler<FollowEventArgs> Followed;
        public event EventHandler<RaidEventArgs> Raided;
        public event EventHandler<RedemptionEventArgs> PointsRedeemed;

        public TwitchEventSubSource(TwitchApiClient api, string broadcasterUserId)
        {
            _api = api;
            _broadcasterUserId = broadcasterUserId;
        }

        public async Task ConnectAsync(CancellationToken ct)
        {
            var services = new ServiceCollection();
            services.AddLogging();
            services.AddTwitchLibEventSubWebsockets();
            _serviceProvider = services.BuildServiceProvider();

            _client = _serviceProvider.GetRequiredService<EventSubWebsocketClient>();

            _client.WebsocketConnected += OnWebsocketConnected;
            _client.WebsocketDisconnected += OnWebsocketDisconnected;
            _client.ErrorOccurred += OnErrorOccurred;

            _client.ChannelFollow += OnChannelFollow;
            _client.ChannelSubscribe += OnChannelSubscribe;
            _client.ChannelSubscriptionGift += OnChannelSubscriptionGift;
            _client.ChannelCheer += OnChannelCheer;
            _client.ChannelRaid += OnChannelRaid;
            _client.ChannelPointsCustomRewardRedemptionAdd += OnRedemption;

            await _client.ConnectAsync().ConfigureAwait(false);
        }

        private async Task OnWebsocketConnected(object sender, WebsocketConnectedArgs e)
        {
            IsConnected = true;
            if (e.IsRequestedReconnect) return;

            // All subscriptions must be created immediately after session_welcome,
            // within Twitch's short window, or the socket gets dropped.
            var sessionId = _client.SessionId;
            var b = _broadcasterUserId;

            await _api.CreateEventSubSubscriptionAsync("channel.follow", "2",
                new Dictionary<string, string> { { "broadcaster_user_id", b }, { "moderator_user_id", b } }, sessionId).ConfigureAwait(false);

            await _api.CreateEventSubSubscriptionAsync("channel.subscribe", "1",
                new Dictionary<string, string> { { "broadcaster_user_id", b } }, sessionId).ConfigureAwait(false);

            await _api.CreateEventSubSubscriptionAsync("channel.subscription.gift", "1",
                new Dictionary<string, string> { { "broadcaster_user_id", b } }, sessionId).ConfigureAwait(false);

            await _api.CreateEventSubSubscriptionAsync("channel.cheer", "1",
                new Dictionary<string, string> { { "broadcaster_user_id", b } }, sessionId).ConfigureAwait(false);

            await _api.CreateEventSubSubscriptionAsync("channel.raid", "1",
                new Dictionary<string, string> { { "to_broadcaster_user_id", b } }, sessionId).ConfigureAwait(false);

            await _api.CreateEventSubSubscriptionAsync("channel.channel_points_custom_reward_redemption.add", "1",
                new Dictionary<string, string> { { "broadcaster_user_id", b } }, sessionId).ConfigureAwait(false);
        }

        private Task OnWebsocketDisconnected(object sender, EventArgs e)
        {
            IsConnected = false;
            return Task.CompletedTask;
        }

        private Task OnErrorOccurred(object sender, ErrorOccuredArgs e)
        {
            System.Diagnostics.Debug.WriteLine($"[ChaosMod] EventSub error: {e.Exception}");
            return Task.CompletedTask;
        }

        private Task OnChannelFollow(object sender, ChannelFollowArgs e)
        {
            var ev = e.Notification.Payload.Event;
            Followed?.Invoke(this, new FollowEventArgs { ViewerId = ev.UserId, Login = ev.UserLogin, DisplayName = ev.UserName });
            return Task.CompletedTask;
        }

        private Task OnChannelSubscribe(object sender, ChannelSubscribeArgs e)
        {
            var ev = e.Notification.Payload.Event;
            Subscribed?.Invoke(this, new SubscriptionEventArgs
            {
                ViewerId = ev.UserId,
                Login = ev.UserLogin,
                DisplayName = ev.UserName,
                Tier = ev.Tier,
                IsGift = ev.IsGift,
                Months = 1
            });
            return Task.CompletedTask;
        }

        private Task OnChannelSubscriptionGift(object sender, ChannelSubscriptionGiftArgs e)
        {
            var ev = e.Notification.Payload.Event;
            Subscribed?.Invoke(this, new SubscriptionEventArgs
            {
                ViewerId = ev.UserId,
                Login = ev.UserLogin,
                DisplayName = ev.UserName,
                Tier = ev.Tier,
                IsGift = true,
                Months = ev.Total
            });
            return Task.CompletedTask;
        }

        private Task OnChannelCheer(object sender, ChannelCheerArgs e)
        {
            var ev = e.Notification.Payload.Event;
            BitsCheered?.Invoke(this, new BitsEventArgs
            {
                ViewerId = ev.IsAnonymous ? null : ev.UserId,
                Login = ev.IsAnonymous ? "anonymous" : ev.UserLogin,
                DisplayName = ev.IsAnonymous ? "Anonymous" : ev.UserName,
                Bits = ev.Bits
            });
            return Task.CompletedTask;
        }

        private Task OnChannelRaid(object sender, ChannelRaidArgs e)
        {
            var ev = e.Notification.Payload.Event;
            Raided?.Invoke(this, new RaidEventArgs
            {
                FromLogin = ev.FromBroadcasterUserLogin,
                FromDisplayName = ev.FromBroadcasterUserName,
                ViewerCount = ev.Viewers
            });
            return Task.CompletedTask;
        }

        private Task OnRedemption(object sender, ChannelPointsCustomRewardRedemptionArgs e)
        {
            var ev = e.Notification.Payload.Event;
            PointsRedeemed?.Invoke(this, new RedemptionEventArgs
            {
                ViewerId = ev.UserId,
                Login = ev.UserLogin,
                DisplayName = ev.UserName,
                RewardTitle = ev.Reward?.Title,
                RewardCost = ev.Reward?.Cost ?? 0,
                UserInput = ev.UserInput
            });
            return Task.CompletedTask;
        }

        public async Task DisconnectAsync()
        {
            if (_client != null)
                await _client.DisconnectAsync().ConfigureAwait(false);
            IsConnected = false;
        }

        public void Dispose()
        {
            try { _client?.DisconnectAsync().Wait(TimeSpan.FromSeconds(2)); } catch { /* ignore */ }
            _serviceProvider?.Dispose();
        }
    }
}
