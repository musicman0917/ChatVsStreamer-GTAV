using System;
using System.IO;
using System.Reflection;
using System.Windows.Forms;
using ChaosMod.Core;
using ChaosMod.GTA.Effects;
using ChaosMod.GTA.Menu;
using GTA;

namespace ChaosMod.GTA
{
    /// <summary>
    /// SHVDN entry point. Keeps almost no logic of its own — everything real
    /// lives in ChaosMod.Core's ChaosModHost, GtaGameContext, and the Effects/
    /// classes. This class exists to: (1) satisfy SHVDN's requirement that a
    /// mod expose a public parameterless-constructible Script subclass, and
    /// (2) be the one place the Tick-thread rule is enforced structurally —
    /// only this class's OnTick ever calls into the effect-execution path.
    /// </summary>
    public sealed class ChaosModScript : Script
    {
        private ChaosModHost _host;
        private GtaGameContext _gameContext;
        private ChaosModMenu _menu;
        private Keys _hotkey = Keys.F9;
        private bool _ready;

        public ChaosModScript()
        {
            Tick += OnTick;
            KeyDown += OnKeyDown;
            Aborted += OnAborted;

            _ = InitializeAsync();
        }

        private async System.Threading.Tasks.Task InitializeAsync()
        {
            try
            {
                var dllPath = Assembly.GetExecutingAssembly().Location;
                var scriptsDir = Path.GetDirectoryName(dllPath) ?? ".";
                var rootPath = Path.Combine(scriptsDir, "ChaosMod");

                _gameContext = new GtaGameContext(rootPath);
                _host = new ChaosModHost(rootPath);
                _host.SetLogger(msg => _gameContext.Log(msg));
                _host.RegisterEffects(EffectFactory.CreateAll());

                await _host.InitializeAsync().ConfigureAwait(false);

                if (!Enum.TryParse(_host.Config.ForceRandomEffectKey, true, out _hotkey))
                    _hotkey = Keys.F9;

                _menu = new ChaosModMenu(_host);
                _ready = true;

                _gameContext.Log("ChaosMod initialized.");
            }
            catch (Exception ex)
            {
                _gameContext?.Log($"ChaosMod failed to initialize: {ex}");
            }
        }

        private void OnTick(object sender, EventArgs e)
        {
            if (!_ready) return;

            // The one place IEffect.Execute/CanExecute are ever invoked — see
            // ChaosModHost.Tick / ShopEngine.DrainAndExecutePending.
            _host.Tick(_gameContext);
            _menu.Process();
        }

        private void OnKeyDown(object sender, KeyEventArgs e)
        {
            if (!_ready) return;

            if (e.KeyCode == Keys.F10 && !e.Alt && !e.Control)
            {
                _menu.Toggle();
                return;
            }

            var requiresCtrl = _host.Config.HotkeyRequireCtrlModifier;
            if (e.KeyCode == _hotkey && (!requiresCtrl || e.Control))
            {
                _host.FireRandomEffect(_gameContext, "hotkey");
            }
        }

        private void OnAborted(object sender, EventArgs e)
        {
            if (!_ready) return;
            _host.Shutdown(_gameContext);
            _host.Dispose();
        }
    }
}
