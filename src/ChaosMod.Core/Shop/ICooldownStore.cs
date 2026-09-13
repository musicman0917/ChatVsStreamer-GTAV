using System;
using System.Collections.Concurrent;

namespace ChaosMod.Core.Shop
{
    /// <summary>
    /// Per-command (global, not per-viewer) cooldown tracking. Deliberately
    /// in-memory only — cooldowns don't need to survive a process restart, so
    /// no SQL table for this.
    /// </summary>
    public interface ICooldownStore
    {
        bool IsOnCooldown(string effectId, TimeSpan cooldown, out TimeSpan remaining);
        void MarkFired(string effectId);
    }

    public sealed class InMemoryCooldownStore : ICooldownStore
    {
        private readonly ConcurrentDictionary<string, DateTime> _lastFiredUtc = new ConcurrentDictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);

        public bool IsOnCooldown(string effectId, TimeSpan cooldown, out TimeSpan remaining)
        {
            if (_lastFiredUtc.TryGetValue(effectId, out var last))
            {
                var elapsed = DateTime.UtcNow - last;
                if (elapsed < cooldown)
                {
                    remaining = cooldown - elapsed;
                    return true;
                }
            }
            remaining = TimeSpan.Zero;
            return false;
        }

        public void MarkFired(string effectId) => _lastFiredUtc[effectId] = DateTime.UtcNow;
    }
}
