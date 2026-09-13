using System;
using System.Numerics;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    /// <summary>
    /// The one gated "ultimate" effect: a combined city-wide chaos burst.
    /// Disabled by default (see EffectRegistry.Register) — the streamer must
    /// explicitly opt in via config.ini or the in-game menu. Explosions are
    /// offset 8-12m from the player and never centered on them, specifically
    /// to avoid an unavoidable instant death; tune further based on playtesting.
    /// </summary>
    public sealed class LosSantosMeltdownEffect : EffectBase
    {
        private readonly Random _random = new Random();

        public LosSantosMeltdownEffect() : base("ultimate.los_santos_meltdown", "meltdown", "Los Santos Meltdown",
            "Combined 5-star wanted level, hostile ped wave, thunderstorm, heavy camera shake, and nearby explosions. Disabled by default.",
            EffectCategory.Ultimate, EffectTier.Ultimate, 5000, TimeSpan.FromMinutes(20)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetWantedLevel(5, true);
            ctx.Peds.SpawnHostilePedsAround(10, 20f);
            ctx.World.SetWeather("ThunderStorm", 2f);
            ctx.Camera.Shake("JOLT_SHAKE", 2.5f);

            var playerPos = ctx.Player.Position;
            var explosionCount = 2 + _random.Next(2); // 2-3

            for (var i = 0; i < explosionCount; i++)
            {
                var angle = _random.NextDouble() * Math.PI * 2;
                var distance = 8f + (float)_random.NextDouble() * 4f; // 8-12m, never centered on the player
                var offset = new Vector3((float)Math.Cos(angle) * distance, (float)Math.Sin(angle) * distance, 0f);
                ctx.World.AddExplosion(playerPos + offset, explosionType: 12 /* EXPLOSION_ROCKET, verify in-game */, cameraShake: 1.5f, isAudible: true, isInvisibleDamage: false);
            }

            return EffectOutcome.Ok();
        }
    }
}
