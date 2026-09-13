using System;

namespace ChaosMod.Core.Abstractions
{
    public enum EffectCategory
    {
        WantedLevel,
        Vehicle,
        Ped,
        PlayerStatus,
        ScreenCamera,
        Economy,
        WeatherTime,
        Blessing,
        Ultimate
    }

    public enum EffectTier
    {
        Nuisance,
        Disruptive,
        Painful,
        Devastating,
        Blessing,
        Ultimate
    }

    public sealed class EffectExecutionContext
    {
        public string ViewerId { get; set; }
        public string ViewerLogin { get; set; }
        public string ViewerDisplayName { get; set; }
        public DateTime TriggeredAtUtc { get; set; }
        public bool ForcedByHotkey { get; set; }
    }

    public sealed class EffectOutcome
    {
        public bool Success { get; }
        public string FailureReason { get; }

        private EffectOutcome(bool success, string failureReason)
        {
            Success = success;
            FailureReason = failureReason;
        }

        public static EffectOutcome Ok() => new EffectOutcome(true, null);
        public static EffectOutcome Fail(string reason) => new EffectOutcome(false, reason);
    }

    /// <summary>
    /// A single purchasable sabotage or blessing. Metadata (id/cost/tier/cooldown)
    /// is plain data usable from Core; CanExecute/Execute bodies that touch native
    /// game calls live in the game-specific project (ChaosMod.GTA) and are only
    /// ever invoked from the tick-thread drain loop in ShopEngine, never directly
    /// from a chat/event callback thread.
    /// </summary>
    public interface IEffect
    {
        string Id { get; }
        string ChatCommand { get; }
        string DisplayName { get; }
        string Description { get; }
        EffectCategory Category { get; }
        EffectTier Tier { get; }
        int DefaultPointCost { get; }
        TimeSpan DefaultCooldown { get; }
        TimeSpan Duration { get; }

        bool CanExecute(IGameContext ctx, out string failureReason);
        EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx);
    }

    /// <summary>
    /// Implemented by effects that need per-frame upkeep (invert controls, temporary
    /// weather, temporary HUD hide, etc.). Registered with StatusEffectRunner on a
    /// successful Execute and ticked every frame until Duration elapses, at which
    /// point Revert is called.
    /// </summary>
    public interface IDurationalEffect : IEffect
    {
        void Tick(IGameContext ctx, TimeSpan elapsed);
        void Revert(IGameContext ctx);
    }
}
