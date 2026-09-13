using System;
using System.Collections.Generic;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.Core.Shop
{
    /// <summary>
    /// Ticks active durational effects (invert controls, temporary weather, etc.)
    /// every frame and reverts them once their Duration elapses. Must only be
    /// driven from the game's Tick thread, same as ShopEngine.DrainAndExecutePending.
    /// </summary>
    public sealed class StatusEffectRunner
    {
        private sealed class ActiveEffect
        {
            public IDurationalEffect Effect;
            public DateTime StartedUtc;
        }

        private readonly List<ActiveEffect> _active = new List<ActiveEffect>();

        public void Register(IDurationalEffect effect)
        {
            _active.Add(new ActiveEffect { Effect = effect, StartedUtc = DateTime.UtcNow });
        }

        public void Tick(IGameContext ctx)
        {
            for (var i = _active.Count - 1; i >= 0; i--)
            {
                var active = _active[i];
                var elapsed = DateTime.UtcNow - active.StartedUtc;

                if (elapsed >= active.Effect.Duration)
                {
                    active.Effect.Revert(ctx);
                    _active.RemoveAt(i);
                    continue;
                }

                active.Effect.Tick(ctx, elapsed);
            }
        }

        public void RevertAll(IGameContext ctx)
        {
            foreach (var active in _active)
                active.Effect.Revert(ctx);
            _active.Clear();
        }
    }
}
