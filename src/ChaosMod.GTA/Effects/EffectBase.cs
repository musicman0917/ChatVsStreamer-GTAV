using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    /// <summary>Boilerplate holder for the plain-data IEffect members shared by every concrete effect.</summary>
    public abstract class EffectBase : IEffect
    {
        public string Id { get; }
        public string ChatCommand { get; }
        public string DisplayName { get; }
        public string Description { get; }
        public EffectCategory Category { get; }
        public EffectTier Tier { get; }
        public int DefaultPointCost { get; }
        public TimeSpan DefaultCooldown { get; }
        public virtual TimeSpan Duration => TimeSpan.Zero;

        protected EffectBase(string id, string chatCommand, string displayName, string description,
            EffectCategory category, EffectTier tier, int defaultPointCost, TimeSpan defaultCooldown)
        {
            Id = id;
            ChatCommand = chatCommand;
            DisplayName = displayName;
            Description = description;
            Category = category;
            Tier = tier;
            DefaultPointCost = defaultPointCost;
            DefaultCooldown = defaultCooldown;
        }

        public virtual bool CanExecute(IGameContext ctx, out string failureReason)
        {
            failureReason = null;
            return true;
        }

        public abstract EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx);
    }
}
