using System;

namespace ChaosMod.Core.Config
{
    /// <summary>
    /// Strongly-typed view over ChaosMod/config.ini. Loaded once at startup;
    /// the LemonUI menu mutates these properties and calls Save() to persist.
    /// </summary>
    public sealed class ChaosModConfig
    {
        private readonly IniFile _ini;

        public string ClientId { get; set; }
        public int OAuthRedirectPort { get; set; }
        public string ChannelName { get; set; }
        public string BotLogin { get; set; }
        public string BotUserId { get; set; }
        public string BroadcasterLogin { get; set; }
        public string BroadcasterUserId { get; set; }

        public int ChatTickIntervalSeconds { get; set; }
        public int PointsPerChatterPerInterval { get; set; }
        public int PointsPerSub { get; set; }
        public int PointsPerGiftSub { get; set; }
        public int PointsPerBit { get; set; }
        public int PointsPerFollow { get; set; }
        public int PointsPerRaidPerViewer { get; set; }
        public int RedemptionPointsMultiplier { get; set; }
        public bool EnableLurkerTrickle { get; set; }
        public int PointsPerLurkerPerInterval { get; set; }

        public bool GlobalRefundOnFailure { get; set; }
        public bool AnnouncePurchasesInChat { get; set; }
        public bool AnnounceRefundsInChat { get; set; }

        public bool OverlayEnabled { get; set; }
        public int OverlayPort { get; set; }
        public bool OverlayBindLoopbackOnly { get; set; }

        public bool ClippingEnabled { get; set; }
        public int ClipDelaySeconds { get; set; }
        public int ClipCooldownSeconds { get; set; }
        public string ClipMinimumTier { get; set; }

        public string ForceRandomEffectKey { get; set; }
        public bool HotkeyRequireCtrlModifier { get; set; }
        public bool HotkeyIncludeUltimateInRandomPool { get; set; }

        private ChaosModConfig(IniFile ini)
        {
            _ini = ini;
        }

        public static ChaosModConfig LoadOrCreate(string path)
        {
            var ini = IniFile.Load(path);
            var cfg = new ChaosModConfig(ini)
            {
                ClientId = ini.GetString("Twitch", "ClientId", ""),
                OAuthRedirectPort = ini.GetInt("Twitch", "OAuthRedirectPort", 3800),
                ChannelName = ini.GetString("Twitch", "ChannelName", ""),
                BotLogin = ini.GetString("Twitch", "BotLogin", ""),
                BotUserId = ini.GetString("Twitch", "BotUserId", ""),
                BroadcasterLogin = ini.GetString("Twitch", "BroadcasterLogin", ""),
                BroadcasterUserId = ini.GetString("Twitch", "BroadcasterUserId", ""),

                ChatTickIntervalSeconds = ini.GetInt("Points", "ChatTickIntervalSeconds", 60),
                PointsPerChatterPerInterval = ini.GetInt("Points", "PointsPerChatterPerInterval", 10),
                PointsPerSub = ini.GetInt("Points", "PointsPerSub", 500),
                PointsPerGiftSub = ini.GetInt("Points", "PointsPerGiftSub", 500),
                PointsPerBit = ini.GetInt("Points", "PointsPerBit", 1),
                PointsPerFollow = ini.GetInt("Points", "PointsPerFollow", 50),
                PointsPerRaidPerViewer = ini.GetInt("Points", "PointsPerRaidPerViewer", 2),
                RedemptionPointsMultiplier = ini.GetInt("Points", "RedemptionPointsMultiplier", 1),
                EnableLurkerTrickle = ini.GetBool("Points", "EnableLurkerTrickle", false),
                PointsPerLurkerPerInterval = ini.GetInt("Points", "PointsPerLurkerPerInterval", 2),

                GlobalRefundOnFailure = ini.GetBool("Shop", "GlobalRefundOnFailure", true),
                AnnouncePurchasesInChat = ini.GetBool("Shop", "AnnouncePurchasesInChat", true),
                AnnounceRefundsInChat = ini.GetBool("Shop", "AnnounceRefundsInChat", true),

                OverlayEnabled = ini.GetBool("Overlay", "Enabled", true),
                OverlayPort = ini.GetInt("Overlay", "Port", 8420),
                OverlayBindLoopbackOnly = ini.GetBool("Overlay", "BindLoopbackOnly", true),

                ClippingEnabled = ini.GetBool("Clipping", "Enabled", true),
                ClipDelaySeconds = ini.GetInt("Clipping", "DelaySeconds", 8),
                ClipCooldownSeconds = ini.GetInt("Clipping", "CooldownSeconds", 60),
                ClipMinimumTier = ini.GetString("Clipping", "MinimumTier", "Disruptive"),

                ForceRandomEffectKey = ini.GetString("Hotkey", "ForceRandomEffectKey", "F9"),
                HotkeyRequireCtrlModifier = ini.GetBool("Hotkey", "RequireCtrlModifier", false),
                HotkeyIncludeUltimateInRandomPool = ini.GetBool("Hotkey", "IncludeUltimateInRandomPool", false)
            };
            return cfg;
        }

        public IniFile RawIni => _ini;

        public void Save(string path)
        {
            _ini.Set("Twitch", "ClientId", ClientId ?? "");
            _ini.Set("Twitch", "OAuthRedirectPort", OAuthRedirectPort.ToString());
            _ini.Set("Twitch", "ChannelName", ChannelName ?? "");
            _ini.Set("Twitch", "BotLogin", BotLogin ?? "");
            _ini.Set("Twitch", "BotUserId", BotUserId ?? "");
            _ini.Set("Twitch", "BroadcasterLogin", BroadcasterLogin ?? "");
            _ini.Set("Twitch", "BroadcasterUserId", BroadcasterUserId ?? "");

            _ini.Set("Points", "ChatTickIntervalSeconds", ChatTickIntervalSeconds.ToString());
            _ini.Set("Points", "PointsPerChatterPerInterval", PointsPerChatterPerInterval.ToString());
            _ini.Set("Points", "PointsPerSub", PointsPerSub.ToString());
            _ini.Set("Points", "PointsPerGiftSub", PointsPerGiftSub.ToString());
            _ini.Set("Points", "PointsPerBit", PointsPerBit.ToString());
            _ini.Set("Points", "PointsPerFollow", PointsPerFollow.ToString());
            _ini.Set("Points", "PointsPerRaidPerViewer", PointsPerRaidPerViewer.ToString());
            _ini.Set("Points", "RedemptionPointsMultiplier", RedemptionPointsMultiplier.ToString());
            _ini.Set("Points", "EnableLurkerTrickle", EnableLurkerTrickle.ToString());
            _ini.Set("Points", "PointsPerLurkerPerInterval", PointsPerLurkerPerInterval.ToString());

            _ini.Set("Shop", "GlobalRefundOnFailure", GlobalRefundOnFailure.ToString());
            _ini.Set("Shop", "AnnouncePurchasesInChat", AnnouncePurchasesInChat.ToString());
            _ini.Set("Shop", "AnnounceRefundsInChat", AnnounceRefundsInChat.ToString());

            _ini.Set("Overlay", "Enabled", OverlayEnabled.ToString());
            _ini.Set("Overlay", "Port", OverlayPort.ToString());
            _ini.Set("Overlay", "BindLoopbackOnly", OverlayBindLoopbackOnly.ToString());

            _ini.Set("Clipping", "Enabled", ClippingEnabled.ToString());
            _ini.Set("Clipping", "DelaySeconds", ClipDelaySeconds.ToString());
            _ini.Set("Clipping", "CooldownSeconds", ClipCooldownSeconds.ToString());
            _ini.Set("Clipping", "MinimumTier", ClipMinimumTier ?? "Disruptive");

            _ini.Set("Hotkey", "ForceRandomEffectKey", ForceRandomEffectKey ?? "F9");
            _ini.Set("Hotkey", "RequireCtrlModifier", HotkeyRequireCtrlModifier.ToString());
            _ini.Set("Hotkey", "IncludeUltimateInRandomPool", HotkeyIncludeUltimateInRandomPool.ToString());

            _ini.Save(path);
        }
    }
}
