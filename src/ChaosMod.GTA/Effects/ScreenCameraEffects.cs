using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class DrunkCamEffect : EffectBase, IDurationalEffect
    {
        public override TimeSpan Duration => TimeSpan.FromSeconds(20);

        public DrunkCamEffect() : base("screen.drunk_cam", "drunk", "Drunk Cam", "Wobbly drunk-camera shake for 20 seconds.",
            EffectCategory.ScreenCamera, EffectTier.Nuisance, 150, TimeSpan.FromSeconds(30)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Camera.Shake("DRUNK_SHAKE", 1.2f);
            return EffectOutcome.Ok();
        }

        public void Tick(IGameContext ctx, TimeSpan elapsed) { /* SHAKE_GAMEPLAY_CAM sustains on its own; nothing to re-apply per frame */ }

        public void Revert(IGameContext ctx) => ctx.Camera.StopShake();
    }

    public sealed class EarthquakeShakeEffect : EffectBase, IDurationalEffect
    {
        public override TimeSpan Duration => TimeSpan.FromSeconds(10);

        public EarthquakeShakeEffect() : base("screen.earthquake", "earthquake", "Earthquake Shake", "Intense camera shake for 10 seconds.",
            EffectCategory.ScreenCamera, EffectTier.Disruptive, 250, TimeSpan.FromSeconds(45)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Camera.Shake("MEDIUM_EXPLOSION_SHAKE", 2.0f);
            return EffectOutcome.Ok();
        }

        public void Tick(IGameContext ctx, TimeSpan elapsed) { }

        public void Revert(IGameContext ctx) => ctx.Camera.StopShake();
    }

    public sealed class HideHudEffect : EffectBase, IDurationalEffect
    {
        public override TimeSpan Duration => TimeSpan.FromSeconds(20);

        public HideHudEffect() : base("screen.hide_hud", "nohud", "Hide HUD & Radar", "Hides the HUD and radar for 20 seconds.",
            EffectCategory.ScreenCamera, EffectTier.Painful, 300, TimeSpan.FromSeconds(60)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetHudVisible(false);
            ctx.Player.SetRadarVisible(false);
            return EffectOutcome.Ok();
        }

        // DISPLAY_HUD/DISPLAY_RADAR only suppress for the current frame, so this
        // must be re-applied every tick for the full duration.
        public void Tick(IGameContext ctx, TimeSpan elapsed)
        {
            ctx.Player.SetHudVisible(false);
            ctx.Player.SetRadarVisible(false);
        }

        public void Revert(IGameContext ctx)
        {
            ctx.Player.SetHudVisible(true);
            ctx.Player.SetRadarVisible(true);
        }
    }
}
