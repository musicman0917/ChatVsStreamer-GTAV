using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class InstantNightEffect : EffectBase
    {
        public InstantNightEffect() : base("weather.instant_night", "night", "Instant Night", "Skips the clock straight to midnight.",
            EffectCategory.WeatherTime, EffectTier.Nuisance, 150, TimeSpan.FromSeconds(45)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.World.SetClockTime(0, 0, 0);
            return EffectOutcome.Ok();
        }
    }

    public sealed class InstantStormEffect : EffectBase
    {
        public InstantStormEffect() : base("weather.instant_storm", "storm", "Instant Storm", "Switches the weather to a thunderstorm.",
            EffectCategory.WeatherTime, EffectTier.Disruptive, 200, TimeSpan.FromSeconds(60)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.World.SetWeather("ThunderStorm", 3f);
            return EffectOutcome.Ok();
        }
    }

    /// <summary>
    /// Durational: always reverts to clear skies rather than "whatever weather
    /// was active before", deliberately — the registry holds one shared
    /// instance per effect, so per-execution mutable state (like a captured
    /// "previous weather") would corrupt itself if this fires again from a
    /// second viewer before the first blizzard's Revert has run (its cooldown
    /// is shorter than its duration, so that overlap is possible).
    /// </summary>
    public sealed class SuddenBlizzardEffect : EffectBase, IDurationalEffect
    {
        public override TimeSpan Duration => TimeSpan.FromMinutes(3);

        public SuddenBlizzardEffect() : base("weather.blizzard", "blizzard", "Sudden Blizzard", "Whites out the screen with a 3-minute blizzard.",
            EffectCategory.WeatherTime, EffectTier.Devastating, 500, TimeSpan.FromSeconds(120)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.World.SetWeather("Blizzard", 2f);
            return EffectOutcome.Ok();
        }

        public void Tick(IGameContext ctx, TimeSpan elapsed) { /* weather persists on its own once set */ }

        public void Revert(IGameContext ctx) => ctx.World.SetWeather("Clear", 3f);
    }
}
