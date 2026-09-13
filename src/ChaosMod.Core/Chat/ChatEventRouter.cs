using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Config;
using ChaosMod.Core.Ledger;
using ChaosMod.Core.Shop;

namespace ChaosMod.Core.Chat
{
    /// <summary>
    /// Normalizes events from any number of IChatSource/IChannelEventSource
    /// implementations into ledger credits and shop purchase attempts. Neither
    /// PointsLedger nor ShopEngine reference IChatSource directly — this is the
    /// one place that wires them together, which is what lets a future
    /// YouTubeLiveChatSource (or FiveM front-end) plug in without touching
    /// either of those.
    /// </summary>
    public sealed class ChatEventRouter : IDisposable
    {
        private readonly PointsLedger _ledger;
        private readonly ShopEngine _shop;
        private readonly ChaosModConfig _config;
        private sealed class RecentChatter
        {
            public DateTime LastSeenUtc;
            public string Login;
        }

        private readonly ConcurrentDictionary<string, RecentChatter> _recentChatters = new ConcurrentDictionary<string, RecentChatter>();
        private Timer _chatTickTimer;

        /// <summary>
        /// Optional: set by ChaosModHost once a broadcaster API client is
        /// available, to support the "lurker trickle" (crediting viewers who are
        /// connected but haven't typed anything). Left null, the trickle is
        /// simply skipped even if EnableLurkerTrickle is on.
        /// </summary>
        public Func<Task<System.Collections.Generic.IReadOnlyList<ActiveChatterInfo>>> ActiveChattersProvider { get; set; }

        public event EventHandler<ChatMessageReceivedEventArgs> AnyMessageObserved;

        public ChatEventRouter(PointsLedger ledger, ShopEngine shop, ChaosModConfig config)
        {
            _ledger = ledger;
            _shop = shop;
            _config = config;
        }

        public void Start()
        {
            var interval = TimeSpan.FromSeconds(Math.Max(15, _config.ChatTickIntervalSeconds));
            _chatTickTimer = new Timer(_ => OnChatTick(), null, interval, interval);
        }

        public void AttachChatSource(IChatSource source)
        {
            source.MessageReceived += OnMessageReceived;
        }

        public void AttachEventSource(IChannelEventSource source)
        {
            source.Subscribed += OnSubscribed;
            source.BitsCheered += OnBitsCheered;
            source.Followed += OnFollowed;
            source.Raided += OnRaided;
            source.PointsRedeemed += OnPointsRedeemed;
        }

        private async void OnMessageReceived(object sender, ChatMessageReceivedEventArgs e)
        {
            try
            {
                await _ledger.EnsureAccountAsync(e.ViewerId, e.Login, e.DisplayName).ConfigureAwait(false);
                _recentChatters[e.ViewerId] = new RecentChatter { LastSeenUtc = DateTime.UtcNow, Login = e.Login };

                AnyMessageObserved?.Invoke(this, e);

                if (ShopEngine.TryParseBuyCommand(e.Message, out var command))
                {
                    await _shop.TryPurchaseAsync(e.ViewerId, e.Login, e.DisplayName, command).ConfigureAwait(false);
                }
            }
            catch (Exception ex)
            {
                // Never let a malformed chat message or transient SQLite error take down the chat client thread.
                System.Diagnostics.Debug.WriteLine($"[ChaosMod] ChatEventRouter.OnMessageReceived error: {ex}");
            }
        }

        private async void OnChatTick()
        {
            try
            {
                var cutoff = DateTime.UtcNow - TimeSpan.FromSeconds(_config.ChatTickIntervalSeconds * 3);
                var creditedThisTick = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase);

                foreach (var kvp in _recentChatters)
                {
                    if (kvp.Value.LastSeenUtc < cutoff)
                    {
                        _recentChatters.TryRemove(kvp.Key, out _);
                        continue;
                    }
                    await _ledger.CreditAsync(kvp.Key, kvp.Value.Login, _config.PointsPerChatterPerInterval, LedgerReason.ChatTick).ConfigureAwait(false);
                    creditedThisTick.Add(kvp.Key);
                }

                if (_config.EnableLurkerTrickle && ActiveChattersProvider != null)
                    await CreditLurkersAsync(creditedThisTick).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[ChaosMod] ChatEventRouter.OnChatTick error: {ex}");
            }
        }

        private async Task CreditLurkersAsync(System.Collections.Generic.HashSet<string> alreadyCreditedViewerIds)
        {
            var chatters = await ActiveChattersProvider().ConfigureAwait(false);
            if (chatters == null) return;

            foreach (var chatter in chatters)
            {
                if (alreadyCreditedViewerIds.Contains(chatter.ViewerId)) continue; // already got the higher chatter rate this tick
                await _ledger.EnsureAccountAsync(chatter.ViewerId, chatter.Login, chatter.DisplayName).ConfigureAwait(false);
                await _ledger.CreditAsync(chatter.ViewerId, chatter.Login, _config.PointsPerLurkerPerInterval, LedgerReason.LurkerTick).ConfigureAwait(false);
            }
        }

        private async void OnSubscribed(object sender, SubscriptionEventArgs e)
        {
            await _ledger.EnsureAccountAsync(e.ViewerId, e.Login, e.DisplayName).ConfigureAwait(false);
            var amount = e.IsGift ? _config.PointsPerGiftSub : _config.PointsPerSub;
            await _ledger.CreditAsync(e.ViewerId, e.Login, amount, e.IsGift ? LedgerReason.GiftSub : LedgerReason.Subscription).ConfigureAwait(false);
        }

        private async void OnBitsCheered(object sender, BitsEventArgs e)
        {
            await _ledger.EnsureAccountAsync(e.ViewerId, e.Login, e.DisplayName).ConfigureAwait(false);
            await _ledger.CreditAsync(e.ViewerId, e.Login, (long)e.Bits * _config.PointsPerBit, LedgerReason.Bits).ConfigureAwait(false);
        }

        private async void OnFollowed(object sender, FollowEventArgs e)
        {
            await _ledger.EnsureAccountAsync(e.ViewerId, e.Login, e.DisplayName).ConfigureAwait(false);
            await _ledger.CreditAsync(e.ViewerId, e.Login, _config.PointsPerFollow, LedgerReason.Follow).ConfigureAwait(false);
        }

        private async void OnRaided(object sender, RaidEventArgs e)
        {
            // Raids don't carry per-viewer identities from Twitch, so this credits
            // the raid leader; per-raider crediting would need the raid party's
            // chatter list, which Twitch does not expose via EventSub.
            var pseudoId = $"raid:{e.FromLogin}";
            await _ledger.EnsureAccountAsync(pseudoId, e.FromLogin, e.FromDisplayName).ConfigureAwait(false);
            await _ledger.CreditAsync(pseudoId, e.FromLogin, (long)e.ViewerCount * _config.PointsPerRaidPerViewer, LedgerReason.Raid).ConfigureAwait(false);
        }

        private async void OnPointsRedeemed(object sender, RedemptionEventArgs e)
        {
            // Channel-point redemptions are an *earning* source (per the original
            // spec), converting Twitch channel points into chaos points — not a
            // way to bypass the "!buy" flow, so this only credits, never spends.
            await _ledger.EnsureAccountAsync(e.ViewerId, e.Login, e.DisplayName).ConfigureAwait(false);
            var amount = (long)e.RewardCost * _config.RedemptionPointsMultiplier;
            await _ledger.CreditAsync(e.ViewerId, e.Login, amount, LedgerReason.Redemption, e.RewardTitle).ConfigureAwait(false);
        }

        public void Dispose()
        {
            _chatTickTimer?.Dispose();
        }
    }
}
