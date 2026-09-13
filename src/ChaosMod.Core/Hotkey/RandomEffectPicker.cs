using System;
using System.Collections.Generic;
using System.Linq;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Shop;

namespace ChaosMod.Core.Hotkey
{
    /// <summary>
    /// Picks a random enabled effect for the force-fire hotkey (clip farming).
    /// Weighted so lower tiers come up more often than Devastating ones; the
    /// Ultimate tier is excluded unless explicitly opted into via ini, since it's
    /// meant to be rare and deliberate, not something a hotkey mashes into chat.
    /// </summary>
    public sealed class RandomEffectPicker
    {
        private readonly Random _random = new Random();

        private static readonly Dictionary<EffectTier, int> Weights = new Dictionary<EffectTier, int>
        {
            { EffectTier.Nuisance, 40 },
            { EffectTier.Disruptive, 30 },
            { EffectTier.Painful, 20 },
            { EffectTier.Devastating, 8 },
            { EffectTier.Blessing, 15 },
            { EffectTier.Ultimate, 1 }
        };

        public EffectDefinition PickRandom(EffectRegistry registry, bool includeUltimate)
        {
            var candidates = registry.Enabled
                .Where(d => includeUltimate || d.Effect.Tier != EffectTier.Ultimate)
                .ToList();

            if (candidates.Count == 0) return null;

            var totalWeight = candidates.Sum(d => Weights.TryGetValue(d.Effect.Tier, out var w) ? w : 10);
            var roll = _random.Next(totalWeight);
            var cumulative = 0;

            foreach (var candidate in candidates)
            {
                cumulative += Weights.TryGetValue(candidate.Effect.Tier, out var w) ? w : 10;
                if (roll < cumulative) return candidate;
            }

            return candidates[candidates.Count - 1];
        }
    }
}
