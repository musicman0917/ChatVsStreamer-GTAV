using System;
using System.Threading;
using System.Threading.Tasks;

namespace ChaosMod.Core.Abstractions
{
    public sealed class ChatMessageReceivedEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
        public string Message { get; set; }
        public bool IsSubscriber { get; set; }
        public bool IsModerator { get; set; }
        public bool IsBroadcaster { get; set; }
        public DateTime ReceivedAtUtc { get; set; } = DateTime.UtcNow;
    }

    /// <summary>
    /// A live chat connection (Twitch today; a future YouTubeLiveChatSource can
    /// implement the same interface without any ledger/shop changes).
    /// Events fire on whatever background thread the underlying client uses —
    /// consumers must never call into GTA/SHVDN APIs directly from a handler.
    /// </summary>
    public interface IChatSource
    {
        string SourceName { get; }
        bool IsConnected { get; }

        event EventHandler<ChatMessageReceivedEventArgs> MessageReceived;

        Task ConnectAsync(CancellationToken ct);
        Task DisconnectAsync();
        Task SendMessageAsync(string message);
    }

    public sealed class SubscriptionEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
        public int Months { get; set; }
        public bool IsGift { get; set; }
        public string Tier { get; set; }
    }

    public sealed class BitsEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
        public int Bits { get; set; }
    }

    public sealed class FollowEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
    }

    public sealed class RaidEventArgs : EventArgs
    {
        public string FromLogin { get; set; }
        public string FromDisplayName { get; set; }
        public int ViewerCount { get; set; }
    }

    public sealed class RedemptionEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
        public string RewardTitle { get; set; }
        public int RewardCost { get; set; }
        public string UserInput { get; set; }
    }

    /// <summary>
    /// Out-of-band channel events (subs/bits/follows/raids/redemptions) separate
    /// from raw chat text, since on Twitch these arrive via EventSub rather than IRC.
    /// </summary>
    public interface IChannelEventSource
    {
        string SourceName { get; }
        bool IsConnected { get; }

        event EventHandler<SubscriptionEventArgs> Subscribed;
        event EventHandler<BitsEventArgs> BitsCheered;
        event EventHandler<FollowEventArgs> Followed;
        event EventHandler<RaidEventArgs> Raided;
        event EventHandler<RedemptionEventArgs> PointsRedeemed;

        Task ConnectAsync(CancellationToken ct);
        Task DisconnectAsync();
    }
}
