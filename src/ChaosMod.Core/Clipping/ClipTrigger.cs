using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Chat.Twitch;
using ChaosMod.Core.Shop;

namespace ChaosMod.Core.Clipping
{
    public sealed class ClipTriggerOptions
    {
        public bool Enabled { get; set; } = true;
        public TimeSpan Delay { get; set; } = TimeSpan.FromSeconds(8);
        public TimeSpan Cooldown { get; set; } = TimeSpan.FromSeconds(60);
        public EffectTier MinimumTier { get; set; } = EffectTier.Disruptive;
    }

    public sealed class ClipCreatedEventArgs : EventArgs
    {
        public string EffectId { get; set; }
        public string EditUrl { get; set; }
    }

    /// <summary>
    /// A few seconds after a sufficiently chaotic effect fires, auto-creates a
    /// Twitch clip via Helix so the moment gets captured without the streamer
    /// lifting a finger. Pure I/O (HTTP calls), driven off a plain Timer rather
    /// than the game Tick thread.
    /// </summary>
    public sealed class ClipTrigger : IDisposable
    {
        private readonly TwitchApiClient _api;
        private readonly ClipTriggerOptions _options;
        private readonly string _broadcasterUserId;
        private readonly ConcurrentQueue<(DateTime fireAtUtc, string effectId)> _pending = new ConcurrentQueue<(DateTime, string)>();
        private readonly Timer _timer;
        private DateTime _lastClipUtc = DateTime.MinValue;

        public event EventHandler<ClipCreatedEventArgs> ClipCreated;

        public ClipTrigger(TwitchApiClient api, ClipTriggerOptions options, string broadcasterUserId)
        {
            _api = api;
            _options = options;
            _broadcasterUserId = broadcasterUserId;
            _timer = new Timer(_ => _ = OnTimerTickAsync(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
        }

        public void NotifyEffectExecuted(EffectExecutedEventArgs e)
        {
            if (!_options.Enabled) return;
            if (e.Definition.Effect.Tier < _options.MinimumTier) return;

            _pending.Enqueue((DateTime.UtcNow + _options.Delay, e.Definition.Effect.Id));
        }

        private async Task OnTimerTickAsync()
        {
            if (_pending.IsEmpty) return;
            if (!_pending.TryPeek(out var next) || next.fireAtUtc > DateTime.UtcNow) return;
            if (!_pending.TryDequeue(out next)) return;

            if (DateTime.UtcNow - _lastClipUtc < _options.Cooldown) return;

            _lastClipUtc = DateTime.UtcNow;
            var result = await _api.CreateClipAsync(_broadcasterUserId).ConfigureAwait(false);
            if (result.Success)
                ClipCreated?.Invoke(this, new ClipCreatedEventArgs { EffectId = next.effectId, EditUrl = result.EditUrl });
            // Failures (e.g. channel not live) are swallowed by design — clipping
            // is a nice-to-have and must never disrupt gameplay or spam retries.
        }

        public void Dispose() => _timer.Dispose();
    }
}
