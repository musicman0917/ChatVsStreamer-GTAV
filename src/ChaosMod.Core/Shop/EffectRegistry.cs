using System;
using System.Collections.Generic;
using System.Linq;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Config;

namespace ChaosMod.Core.Shop
{
    public sealed class EffectDefinition
    {
        public IEffect Effect { get; }
        public bool Enabled { get; set; }
        public int Cost { get; set; }
        public TimeSpan Cooldown { get; set; }

        public EffectDefinition(IEffect effect, bool enabled, int cost, TimeSpan cooldown)
        {
            Effect = effect;
            Enabled = enabled;
            Cost = cost;
            Cooldown = cooldown;
        }
    }

    /// <summary>
    /// Holds all known effects and their live (ini-overridable) cost/cooldown/enabled
    /// settings. Effects themselves are registered manually (a fixed, narrow roster),
    /// no reflection scanning needed.
    /// </summary>
    public sealed class EffectRegistry
    {
        private readonly Dictionary<string, EffectDefinition> _byId = new Dictionary<string, EffectDefinition>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, EffectDefinition> _byCommand = new Dictionary<string, EffectDefinition>(StringComparer.OrdinalIgnoreCase);

        public void Register(IEffect effect)
        {
            // Ultimate-tier effects are opt-in only: extreme cost/cooldown alone
            // isn't enough of a gate, so they also default to disabled until the
            // streamer explicitly flips them on in config.ini or the in-game menu.
            var defaultEnabled = effect.Tier != EffectTier.Ultimate;
            var def = new EffectDefinition(effect, defaultEnabled, effect.DefaultPointCost, effect.DefaultCooldown);
            _byId[effect.Id] = def;
            _byCommand[NormalizeCommand(effect.ChatCommand)] = def;
        }

        public void ApplyIniOverrides(IniFile ini)
        {
            foreach (var def in _byId.Values)
            {
                var id = def.Effect.Id;
                def.Enabled = ini.GetBool("Effects", $"{id}.Enabled", def.Enabled);
                def.Cost = ini.GetInt("Effects", $"{id}.Cost", def.Cost);
                var cooldownSeconds = ini.GetInt("Effects", $"{id}.CooldownSeconds", (int)def.Cooldown.TotalSeconds);
                def.Cooldown = TimeSpan.FromSeconds(cooldownSeconds);
            }
        }

        public void WriteDefaultsIntoIni(IniFile ini)
        {
            foreach (var def in _byId.Values.OrderBy(d => d.Effect.Category).ThenBy(d => d.Effect.Id))
            {
                var id = def.Effect.Id;
                if (ini.GetRaw("Effects", $"{id}.Enabled") == null)
                    ini.Set("Effects", $"{id}.Enabled", def.Enabled.ToString());
                if (ini.GetRaw("Effects", $"{id}.Cost") == null)
                    ini.Set("Effects", $"{id}.Cost", def.Cost.ToString());
                if (ini.GetRaw("Effects", $"{id}.CooldownSeconds") == null)
                    ini.Set("Effects", $"{id}.CooldownSeconds", ((int)def.Cooldown.TotalSeconds).ToString());
            }
        }

        public bool TryGetByCommand(string chatCommand, out EffectDefinition definition)
            => _byCommand.TryGetValue(NormalizeCommand(chatCommand), out definition);

        public bool TryGetById(string id, out EffectDefinition definition)
            => _byId.TryGetValue(id, out definition);

        public IReadOnlyCollection<EffectDefinition> All => _byId.Values;

        public IEnumerable<EffectDefinition> Enabled => _byId.Values.Where(d => d.Enabled);

        private static string NormalizeCommand(string command)
        {
            if (string.IsNullOrWhiteSpace(command)) return string.Empty;
            var trimmed = command.Trim();
            return trimmed.StartsWith("!") ? trimmed.Substring(1) : trimmed;
        }
    }
}
