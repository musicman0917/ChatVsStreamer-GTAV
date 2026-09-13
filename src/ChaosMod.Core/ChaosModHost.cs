using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Chat;
using ChaosMod.Core.Chat.Twitch;
using ChaosMod.Core.Clipping;
using ChaosMod.Core.Config;
using ChaosMod.Core.Hotkey;
using ChaosMod.Core.Ledger;
using ChaosMod.Core.Overlay;
using ChaosMod.Core.Shop;

namespace ChaosMod.Core
{
    /// <summary>
    /// Composition root for everything in ChaosMod.Core. The GTA-specific entry
    /// point (ChaosModScript) constructs one of these, hands it the concrete
    /// IEffect list and an IGameContext, and calls Tick()/Dispose() at the
    /// right times. Nothing in this class references GTA/SHVDN.
    /// </summary>
    public sealed class ChaosModHost : IDisposable
    {
        public string RootPath { get; }
        public ChaosModConfig Config { get; private set; }
        public EffectRegistry Registry { get; } = new EffectRegistry();
        public PointsLedger Ledger { get; private set; }
        public ShopEngine Shop { get; private set; }
        public StatusEffectRunner StatusEffects { get; } = new StatusEffectRunner();
        public RandomEffectPicker RandomPicker { get; } = new RandomEffectPicker();
        public OverlayServer Overlay { get; private set; }
        public TwitchOAuthService OAuth { get; private set; }
        public TwitchTokenStore TokenStore { get; private set; }
        public ChatEventRouter ChatRouter { get; private set; }
        public ClipTrigger Clipper { get; private set; }

        private readonly ChatSourceResponder _chatResponder = new ChatSourceResponder();
        private readonly ICooldownStore _cooldowns = new InMemoryCooldownStore();
        private readonly EffectExecutionQueue _queue = new EffectExecutionQueue();
        private SqliteLedgerStore _ledgerStore;
        private TwitchChatSource _chatSource;
        private TwitchEventSubSource _eventSubSource;
        private TwitchApiClient _apiClient;

        private Action<string> _logger = _ => { };

        public ChaosModHost(string rootPath)
        {
            RootPath = rootPath;
        }

        public void SetLogger(Action<string> logger) => _logger = logger ?? (_ => { });

        public void RegisterEffects(IEnumerable<IEffect> effects)
        {
            foreach (var effect in effects) Registry.Register(effect);
        }

        /// <summary>
        /// Loads config, opens the SQLite ledger, wires the shop engine, starts
        /// the overlay server, and — if valid tokens are already stored — starts
        /// the Twitch chat/EventSub connections. Call once at mod startup, after
        /// RegisterEffects.
        /// </summary>
        public async Task InitializeAsync()
        {
            Directory.CreateDirectory(RootPath);

            var configPath = Path.Combine(RootPath, "config.ini");
            Config = ChaosModConfig.LoadOrCreate(configPath);
            Registry.WriteDefaultsIntoIni(Config.RawIni);
            Config.Save(configPath);
            Registry.ApplyIniOverrides(Config.RawIni);

            _ledgerStore = new SqliteLedgerStore(Path.Combine(RootPath, "chaosmod.sqlite"));
            _ledgerStore.Initialize();
            Ledger = new PointsLedger(_ledgerStore);

            Shop = new ShopEngine(Registry, Ledger, _cooldowns, _queue, _chatResponder, new ShopOptions
            {
                GlobalRefundOnFailure = Config.GlobalRefundOnFailure,
                AnnouncePurchasesInChat = Config.AnnouncePurchasesInChat,
                AnnounceRefundsInChat = Config.AnnounceRefundsInChat
            });

            ChatRouter = new ChatEventRouter(Ledger, Shop, Config);
            ChatRouter.Start();

            TokenStore = new TwitchTokenStore(Path.Combine(RootPath, "tokens.json"));
            OAuth = new TwitchOAuthService(new TwitchOAuthOptions { ClientId = Config.ClientId, RedirectPort = Config.OAuthRedirectPort }, TokenStore);

            Overlay = new OverlayServer(new OverlayOptions
            {
                Enabled = Config.OverlayEnabled,
                Port = Config.OverlayPort,
                BindLoopbackOnly = Config.OverlayBindLoopbackOnly,
                StaticRootPath = Path.Combine(RootPath, "overlay")
            })
            {
                BuildSnapshot = BuildSnapshot
            };

            if (Config.OverlayEnabled)
            {
                try { await Overlay.StartAsync().ConfigureAwait(false); }
                catch (Exception ex) { _logger($"Overlay server failed to start: {ex.Message}"); }
            }

            Shop.EffectExecuted += OnEffectExecuted;
            Shop.EffectFailed += OnEffectFailed;
            Shop.PurchaseAttempted += OnPurchaseAttempted;
            Ledger.BalanceChanged += OnBalanceChanged;
            ChatRouter.AnyMessageObserved += OnAnyMessageObserved;

            await TryStartTwitchAsync().ConfigureAwait(false);
        }

        private async Task TryStartTwitchAsync()
        {
            var botToken = TokenStore.Get(TwitchTokenRole.Bot);
            var broadcasterToken = TokenStore.Get(TwitchTokenRole.Broadcaster);

            if (botToken != null && !string.IsNullOrEmpty(Config.ChannelName))
            {
                try
                {
                    _chatSource = new TwitchChatSource(botToken.Login, botToken.AccessToken, Config.ChannelName);
                    ChatRouter.AttachChatSource(_chatSource);
                    _chatResponder.ActiveSource = _chatSource;
                    await _chatSource.ConnectAsync(CancellationToken.None).ConfigureAwait(false);
                }
                catch (Exception ex) { _logger($"Twitch chat connect failed: {ex.Message}"); }
            }

            if (broadcasterToken != null)
            {
                try
                {
                    _apiClient = new TwitchApiClient(Config.ClientId, broadcasterToken.AccessToken);

                    _eventSubSource = new TwitchEventSubSource(_apiClient, broadcasterToken.UserId);
                    ChatRouter.AttachEventSource(_eventSubSource);
                    await _eventSubSource.ConnectAsync(CancellationToken.None).ConfigureAwait(false);

                    // Broadcaster counts as their own moderator for this endpoint.
                    ChatRouter.ActiveChattersProvider = async () =>
                    {
                        var chatters = await _apiClient.GetChattersAsync(broadcasterToken.UserId, broadcasterToken.UserId).ConfigureAwait(false);
                        return chatters.Select(c => new ActiveChatterInfo { ViewerId = c.UserId, Login = c.Login, DisplayName = c.DisplayName }).ToList();
                    };

                    Clipper = new ClipTrigger(_apiClient, new ClipTriggerOptions
                    {
                        Enabled = Config.ClippingEnabled,
                        Delay = TimeSpan.FromSeconds(Config.ClipDelaySeconds),
                        Cooldown = TimeSpan.FromSeconds(Config.ClipCooldownSeconds),
                        MinimumTier = Enum.TryParse<EffectTier>(Config.ClipMinimumTier, true, out var tier) ? tier : EffectTier.Disruptive
                    }, broadcasterToken.UserId);
                    Clipper.ClipCreated += OnClipCreated;
                }
                catch (Exception ex) { _logger($"Twitch EventSub connect failed: {ex.Message}"); }
            }
        }

        /// <summary>Must be called every frame from the game's own Tick thread — never from anywhere else.</summary>
        public void Tick(IGameContext ctx)
        {
            Shop.DrainAndExecutePending(ctx);
            StatusEffects.Tick(ctx);
        }

        /// <summary>Call after any menu edit that changes cost/cooldown/enabled state, so the overlay's shop list stays live.</summary>
        public void BroadcastCatalog()
        {
            var catalog = Registry.All.Select(d => new ShopCommandDto
            {
                Id = d.Effect.Id,
                Command = "!buy " + d.Effect.ChatCommand.TrimStart('!'),
                DisplayName = d.Effect.DisplayName,
                Category = d.Effect.Category.ToString(),
                Tier = d.Effect.Tier.ToString(),
                Cost = d.Cost,
                CooldownSeconds = (int)d.Cooldown.TotalSeconds,
                Enabled = d.Enabled
            }).ToList();
            Overlay.Broadcast(new ShopCatalogDto { Commands = catalog });
        }

        public void FireRandomEffect(IGameContext ctx, string label)
        {
            var def = RandomPicker.PickRandom(Registry, Config.HotkeyIncludeUltimateInRandomPool);
            if (def == null) { _logger("No enabled effects available for the hotkey."); return; }
            Shop.EnqueueForced(def, label);
        }

        private void OnEffectExecuted(object sender, EffectExecutedEventArgs e)
        {
            Overlay.Broadcast(new EffectFiredDto
            {
                Ts = DateTime.UtcNow,
                ViewerLogin = e.ExecutionContext.ViewerLogin,
                EffectId = e.Definition.Effect.Id,
                DisplayName = e.Definition.Effect.DisplayName,
                Tier = e.Definition.Effect.Tier.ToString(),
                ForcedByHotkey = e.ExecutionContext.ForcedByHotkey
            });
            Clipper?.NotifyEffectExecuted(e);

            if (e.Definition.Effect is IDurationalEffect durational)
                StatusEffects.Register(durational);
        }

        private void OnEffectFailed(object sender, EffectFailedEventArgs e)
        {
            if (!e.Refunded) return;
            Overlay.Broadcast(new RefundDto
            {
                Ts = DateTime.UtcNow,
                ViewerLogin = e.ExecutionContext.ViewerLogin,
                Amount = e.Definition.Cost,
                EffectId = e.Definition.Effect.Id,
                Reason = e.Reason
            });
        }

        private void OnPurchaseAttempted(object sender, PurchaseAttemptedEventArgs e)
        {
            var result = e.Result;
            Overlay.Broadcast(new PurchaseDto
            {
                Ts = DateTime.UtcNow,
                ViewerLogin = result.ViewerLogin,
                EffectId = result.Definition?.Effect.Id,
                DisplayName = result.Definition?.Effect.DisplayName,
                Tier = result.Definition?.Effect.Tier.ToString(),
                Cost = result.Definition?.Cost ?? 0,
                Success = result.Accepted
            });
        }

        private void OnBalanceChanged(object sender, BalanceChangedEventArgs e)
        {
            if (e.Login == null) return;
            Overlay.Broadcast(new BalanceUpdateDto { ViewerLogin = e.Login, Balance = e.Balance });

            // Cheap enough for a personal-stream-scale audience; refreshes the
            // overlay's leaderboard on every earn/spend rather than polling.
            var leaderboard = Ledger.GetLeaderboardAsync(10).GetAwaiter().GetResult()
                .Select(r => new LeaderboardEntryDto { Rank = r.Rank, ViewerLogin = r.Login, Balance = r.Balance })
                .ToList();
            Overlay.Broadcast(new LeaderboardDto { Entries = leaderboard });
        }

        private void OnAnyMessageObserved(object sender, ChatMessageReceivedEventArgs e)
        {
            Overlay.Broadcast(new ChatMessageDto
            {
                Ts = e.ReceivedAtUtc,
                ViewerLogin = e.Login,
                DisplayName = e.DisplayName,
                Message = e.Message,
                IsSub = e.IsSubscriber,
                IsMod = e.IsModerator
            });
        }

        private void OnClipCreated(object sender, ClipCreatedEventArgs e)
        {
            Overlay.Broadcast(new ClipCreatedDto { EffectId = e.EffectId, Url = e.EditUrl });
        }

        private SnapshotDto BuildSnapshot()
        {
            var catalog = Registry.All.Select(d => new ShopCommandDto
            {
                Id = d.Effect.Id,
                Command = "!buy " + d.Effect.ChatCommand.TrimStart('!'),
                DisplayName = d.Effect.DisplayName,
                Category = d.Effect.Category.ToString(),
                Tier = d.Effect.Tier.ToString(),
                Cost = d.Cost,
                CooldownSeconds = (int)d.Cooldown.TotalSeconds,
                Enabled = d.Enabled
            }).ToList();

            var leaderboard = Ledger.GetLeaderboardAsync(10).GetAwaiter().GetResult()
                .Select(r => new LeaderboardEntryDto { Rank = r.Rank, ViewerLogin = r.Login, Balance = r.Balance })
                .ToList();

            return new SnapshotDto { Catalog = catalog, Leaderboard = leaderboard, RecentChat = new List<ChatMessageDto>() };
        }

        /// <summary>
        /// Call from the SHVDN script's Aborted handler, while the real
        /// IGameContext is still valid, so any active durational effect
        /// (inverted controls, hidden HUD, altered weather, ...) gets properly
        /// reverted instead of being left stuck when the mod unloads.
        /// </summary>
        public void Shutdown(IGameContext ctx)
        {
            try { StatusEffects.RevertAll(ctx); } catch (Exception ex) { _logger($"Error reverting active effects on shutdown: {ex.Message}"); }
        }

        public void Dispose()
        {
            Clipper?.Dispose();
            _eventSubSource?.Dispose();
            _chatSource?.Dispose();
            ChatRouter?.Dispose();
            OAuth?.Dispose();
            Overlay?.Dispose();
            _ledgerStore?.Dispose();
        }
    }
}
