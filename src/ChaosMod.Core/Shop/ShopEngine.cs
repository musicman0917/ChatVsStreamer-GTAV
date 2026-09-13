using System;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Ledger;

namespace ChaosMod.Core.Shop
{
    public enum PurchaseFailureKind
    {
        None,
        UnknownCommand,
        EffectDisabled,
        OnCooldown,
        InsufficientBalance
    }

    public sealed class PurchaseResult
    {
        public bool Accepted { get; set; }
        public PurchaseFailureKind FailureKind { get; set; }
        public string Message { get; set; }
        public EffectDefinition Definition { get; set; }
        public string ViewerLogin { get; set; }
    }

    public sealed class PurchaseAttemptedEventArgs : EventArgs
    {
        public PurchaseResult Result { get; set; }
    }

    public sealed class EffectExecutedEventArgs : EventArgs
    {
        public EffectDefinition Definition { get; set; }
        public EffectExecutionContext ExecutionContext { get; set; }
    }

    public sealed class EffectFailedEventArgs : EventArgs
    {
        public EffectDefinition Definition { get; set; }
        public EffectExecutionContext ExecutionContext { get; set; }
        public string Reason { get; set; }
        public bool Refunded { get; set; }
    }

    public sealed class ShopOptions
    {
        public bool GlobalRefundOnFailure { get; set; } = true;
        public bool AnnouncePurchasesInChat { get; set; } = true;
        public bool AnnounceRefundsInChat { get; set; } = true;
    }

    /// <summary>
    /// Parses "!buy &lt;command&gt;", validates against balance/cooldown/enabled
    /// state, performs the atomic debit, and enqueues the effect for execution
    /// on the next Tick. DrainAndExecutePending must only ever be called from
    /// the game's Tick thread.
    /// </summary>
    public sealed class ShopEngine
    {
        private readonly EffectRegistry _registry;
        private readonly PointsLedger _ledger;
        private readonly ICooldownStore _cooldowns;
        private readonly EffectExecutionQueue _queue;
        private readonly IChatResponder _responder;
        private readonly ShopOptions _options;

        public event EventHandler<EffectExecutedEventArgs> EffectExecuted;
        public event EventHandler<EffectFailedEventArgs> EffectFailed;
        public event EventHandler<PurchaseAttemptedEventArgs> PurchaseAttempted;

        public ShopEngine(EffectRegistry registry, PointsLedger ledger, ICooldownStore cooldowns,
            EffectExecutionQueue queue, IChatResponder responder, ShopOptions options)
        {
            _registry = registry;
            _ledger = ledger;
            _cooldowns = cooldowns;
            _queue = queue;
            _responder = responder;
            _options = options;
        }

        public static bool TryParseBuyCommand(string chatMessage, out string command)
        {
            command = null;
            if (string.IsNullOrWhiteSpace(chatMessage)) return false;
            var trimmed = chatMessage.Trim();
            if (!trimmed.StartsWith("!buy ", StringComparison.OrdinalIgnoreCase)) return false;
            command = trimmed.Substring(5).Trim();
            return command.Length > 0;
        }

        public async Task<PurchaseResult> TryPurchaseAsync(string viewerId, string viewerLogin, string viewerDisplayName, string chatCommand)
        {
            if (!_registry.TryGetByCommand(chatCommand, out var def))
            {
                return await RespondAsync(new PurchaseResult
                {
                    Accepted = false,
                    FailureKind = PurchaseFailureKind.UnknownCommand,
                    Message = $"@{viewerDisplayName} unknown command '!buy {chatCommand}'.",
                    ViewerLogin = viewerLogin
                }).ConfigureAwait(false);
            }

            if (!def.Enabled)
            {
                return await RespondAsync(new PurchaseResult
                {
                    Accepted = false,
                    FailureKind = PurchaseFailureKind.EffectDisabled,
                    Message = $"@{viewerDisplayName} '{def.Effect.DisplayName}' is currently disabled.",
                    Definition = def,
                    ViewerLogin = viewerLogin
                }).ConfigureAwait(false);
            }

            if (_cooldowns.IsOnCooldown(def.Effect.Id, def.Cooldown, out var remaining))
            {
                return await RespondAsync(new PurchaseResult
                {
                    Accepted = false,
                    FailureKind = PurchaseFailureKind.OnCooldown,
                    Message = $"@{viewerDisplayName} '{def.Effect.DisplayName}' is on cooldown for {Math.Ceiling(remaining.TotalSeconds)}s.",
                    Definition = def,
                    ViewerLogin = viewerLogin
                }).ConfigureAwait(false);
            }

            var debit = await _ledger.TryDebitAsync(viewerId, viewerLogin, def.Cost, LedgerReason.ShopPurchase, def.Effect.Id).ConfigureAwait(false);
            if (!debit.Ok)
            {
                return await RespondAsync(new PurchaseResult
                {
                    Accepted = false,
                    FailureKind = PurchaseFailureKind.InsufficientBalance,
                    Message = $"@{viewerDisplayName} you need {def.Cost} points for '{def.Effect.DisplayName}' (you have {debit.NewBalance}).",
                    Definition = def,
                    ViewerLogin = viewerLogin
                }).ConfigureAwait(false);
            }

            // Claim the cooldown immediately on purchase (not on execute) so a burst of
            // near-simultaneous buys for the same command can't all slip through before
            // the tick thread drains the queue.
            _cooldowns.MarkFired(def.Effect.Id);

            var execCtx = new EffectExecutionContext
            {
                ViewerId = viewerId,
                ViewerLogin = viewerLogin,
                ViewerDisplayName = viewerDisplayName,
                TriggeredAtUtc = DateTime.UtcNow,
                ForcedByHotkey = false
            };

            _queue.Enqueue(new QueuedEffectExecution(def, execCtx, debit.TransactionId));

            var result = new PurchaseResult
            {
                Accepted = true,
                FailureKind = PurchaseFailureKind.None,
                Message = $"@{viewerDisplayName} bought '{def.Effect.DisplayName}' for {def.Cost} points!",
                Definition = def,
                ViewerLogin = viewerLogin
            };

            if (_options.AnnouncePurchasesInChat)
                await SafeRespond(result.Message).ConfigureAwait(false);

            PurchaseAttempted?.Invoke(this, new PurchaseAttemptedEventArgs { Result = result });
            return result;
        }

        public void EnqueueForced(EffectDefinition def, string viewerLabel)
        {
            var execCtx = new EffectExecutionContext
            {
                ViewerId = "hotkey",
                ViewerLogin = viewerLabel,
                ViewerDisplayName = viewerLabel,
                TriggeredAtUtc = DateTime.UtcNow,
                ForcedByHotkey = true
            };
            _queue.Enqueue(new QueuedEffectExecution(def, execCtx, null));
        }

        /// <summary>
        /// Must only be called from the game's own Tick thread.
        /// </summary>
        public void DrainAndExecutePending(IGameContext ctx)
        {
            while (_queue.TryDequeue(out var pending))
            {
                Execute(ctx, pending);
            }
        }

        private void Execute(IGameContext ctx, QueuedEffectExecution pending)
        {
            var def = pending.Definition;
            var execCtx = pending.ExecutionContext;

            if (!def.Effect.CanExecute(ctx, out var failureReason))
            {
                HandleFailure(ctx, def, execCtx, failureReason ?? "conditions not met");
                return;
            }

            var outcome = def.Effect.Execute(ctx, execCtx);
            if (!outcome.Success)
            {
                HandleFailure(ctx, def, execCtx, outcome.FailureReason ?? "execution failed");
                return;
            }

            EffectExecuted?.Invoke(this, new EffectExecutedEventArgs { Definition = def, ExecutionContext = execCtx });
        }

        private void HandleFailure(IGameContext ctx, EffectDefinition def, EffectExecutionContext execCtx, string reason)
        {
            var refunded = false;
            if (_options.GlobalRefundOnFailure && !execCtx.ForcedByHotkey)
            {
                refunded = true;
                // Fire-and-forget refund: this is pure SQL I/O, safe to run off the tick thread.
                _ = _ledger.RefundAsync(execCtx.ViewerId, execCtx.ViewerLogin, def.Cost, def.Effect.Id, reason)
                    .ContinueWith(t =>
                    {
                        if (t.IsFaulted) ctx.Log($"[ChaosMod] Refund failed for {execCtx.ViewerLogin}: {t.Exception?.Message}");
                    });

                if (_options.AnnounceRefundsInChat)
                {
                    var msg = $"@{execCtx.ViewerDisplayName} '{def.Effect.DisplayName}' couldn't fire ({reason}) — refunded {def.Cost} points.";
                    _ = SafeRespond(msg);
                }
            }

            ctx.Log($"[ChaosMod] Effect '{def.Effect.Id}' failed for {execCtx.ViewerLogin}: {reason}");
            EffectFailed?.Invoke(this, new EffectFailedEventArgs { Definition = def, ExecutionContext = execCtx, Reason = reason, Refunded = refunded });
        }

        private async Task<PurchaseResult> RespondAsync(PurchaseResult result)
        {
            await SafeRespond(result.Message).ConfigureAwait(false);
            PurchaseAttempted?.Invoke(this, new PurchaseAttemptedEventArgs { Result = result });
            return result;
        }

        private async Task SafeRespond(string message)
        {
            if (_responder == null) return;
            try { await _responder.SendAsync(message).ConfigureAwait(false); }
            catch { /* chat responder failures must never break the purchase flow */ }
        }
    }
}
