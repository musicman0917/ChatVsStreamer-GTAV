using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core;
using ChaosMod.Core.Chat.Twitch;
using LemonUI;
using LemonUI.Menus;

namespace ChaosMod.GTA.Menu
{
    /// <summary>
    /// LemonUI-based in-game menu: OAuth trigger buttons, live-adjustable
    /// toggles, and per-effect enable/disable. Fine-grained cost/cooldown
    /// tuning is done in config.ini (which this menu keeps in sync via
    /// ChaosModConfig.Save) rather than duplicated as sliders here.
    /// </summary>
    public sealed class ChaosModMenu
    {
        public ObjectPool Pool { get; } = new ObjectPool();

        private readonly NativeMenu _mainMenu = new NativeMenu("Chaos Mod", "Chat vs Streamer");
        private readonly NativeItem _authBotItem;
        private readonly NativeItem _authBroadcasterItem;
        private readonly ChaosModHost _host;

        public ChaosModMenu(ChaosModHost host)
        {
            _host = host;

            Pool.Add(_mainMenu);

            _authBotItem = new NativeItem("Authorize Bot Account", BuildBotStatus());
            _authBotItem.Activated += (s, e) => _ = AuthorizeAsync(TwitchTokenRole.Bot);
            _mainMenu.Add(_authBotItem);

            _authBroadcasterItem = new NativeItem("Authorize Broadcaster Account", BuildBroadcasterStatus());
            _authBroadcasterItem.Activated += (s, e) => _ = AuthorizeAsync(TwitchTokenRole.Broadcaster);
            _mainMenu.Add(_authBroadcasterItem);

            var overlayToggle = new NativeCheckboxItem("Overlay Enabled", host.Config.OverlayEnabled);
            overlayToggle.CheckboxChanged += (s, e) => { host.Config.OverlayEnabled = overlayToggle.Checked; SaveConfig(); };
            _mainMenu.Add(overlayToggle);

            var announcePurchases = new NativeCheckboxItem("Announce Purchases in Chat", host.Config.AnnouncePurchasesInChat);
            announcePurchases.CheckboxChanged += (s, e) => { host.Config.AnnouncePurchasesInChat = announcePurchases.Checked; SaveConfig(); };
            _mainMenu.Add(announcePurchases);

            var announceRefunds = new NativeCheckboxItem("Announce Refunds in Chat", host.Config.AnnounceRefundsInChat);
            announceRefunds.CheckboxChanged += (s, e) => { host.Config.AnnounceRefundsInChat = announceRefunds.Checked; SaveConfig(); };
            _mainMenu.Add(announceRefunds);

            var lurkerTrickle = new NativeCheckboxItem("Lurker Point Trickle", host.Config.EnableLurkerTrickle);
            lurkerTrickle.CheckboxChanged += (s, e) => { host.Config.EnableLurkerTrickle = lurkerTrickle.Checked; SaveConfig(); };
            _mainMenu.Add(lurkerTrickle);

            var effectsMenu = BuildEffectsSubmenu();
            _mainMenu.AddSubMenu(effectsMenu);
            Pool.Add(effectsMenu);
        }

        private NativeMenu BuildEffectsSubmenu()
        {
            var menu = new NativeMenu("Chaos Mod", "Effects");
            foreach (var def in _host.Registry.All)
            {
                var checkbox = new NativeCheckboxItem($"{def.Effect.DisplayName} ({def.Cost}pts / {(int)def.Cooldown.TotalSeconds}s)", def.Enabled)
                {
                    Description = def.Effect.Description
                };
                checkbox.CheckboxChanged += (s, e) => { def.Enabled = checkbox.Checked; SaveConfig(); _host.BroadcastCatalog(); };
                menu.Add(checkbox);
            }
            return menu;
        }

        private async Task AuthorizeAsync(TwitchTokenRole role)
        {
            var item = role == TwitchTokenRole.Bot ? _authBotItem : _authBroadcasterItem;
            item.Enabled = false;
            item.Description = "Waiting for browser authorization…";

            try
            {
                var result = role == TwitchTokenRole.Bot
                    ? await _host.OAuth.AuthorizeBotAccountAsync(CancellationToken.None).ConfigureAwait(false)
                    : await _host.OAuth.AuthorizeBroadcasterAccountAsync(CancellationToken.None).ConfigureAwait(false);

                if (result.Success)
                {
                    if (role == TwitchTokenRole.Bot)
                    {
                        _host.Config.BotLogin = result.Token.Login;
                        _host.Config.BotUserId = result.Token.UserId;
                    }
                    else
                    {
                        _host.Config.BroadcasterLogin = result.Token.Login;
                        _host.Config.BroadcasterUserId = result.Token.UserId;
                    }
                    SaveConfig();
                    item.Description = $"Authorized as {result.Token.Login}. Restart the mod to connect.";
                }
                else
                {
                    item.Description = $"Failed: {result.Error}";
                }
            }
            finally
            {
                item.Enabled = true;
            }
        }

        private string BuildBotStatus() =>
            string.IsNullOrEmpty(_host.Config.BotLogin) ? "Not authorized." : $"Authorized as {_host.Config.BotLogin}.";

        private string BuildBroadcasterStatus() =>
            string.IsNullOrEmpty(_host.Config.BroadcasterLogin) ? "Not authorized." : $"Authorized as {_host.Config.BroadcasterLogin}.";

        private void SaveConfig() => _host.Config.Save(Path.Combine(_host.RootPath, "config.ini"));

        public void Toggle() => _mainMenu.Visible = !_mainMenu.Visible;

        public void Process() => Pool.Process();
    }
}
