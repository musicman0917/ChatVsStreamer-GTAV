using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace ChaosMod.Core.Ledger
{
    /// <summary>
    /// Public-facing points economy API. Thin wrapper over ILedgerStore that
    /// also raises BalanceChanged so the overlay can push live balance updates.
    /// </summary>
    public sealed class PointsLedger
    {
        private readonly ILedgerStore _store;

        public event EventHandler<BalanceChangedEventArgs> BalanceChanged;

        public PointsLedger(ILedgerStore store)
        {
            _store = store ?? throw new ArgumentNullException(nameof(store));
        }

        public void Initialize() => _store.Initialize();

        public Task EnsureAccountAsync(string viewerId, string login, string displayName)
            => _store.EnsureAccountAsync(viewerId, login, displayName);

        public Task<long> GetBalanceAsync(string viewerId) => _store.GetBalanceAsync(viewerId);

        public async Task<long> CreditAsync(string viewerId, string login, long amount, LedgerReason reason, string note = null)
        {
            if (amount <= 0) return await _store.GetBalanceAsync(viewerId).ConfigureAwait(false);

            var newBalance = await _store.CreditAsync(viewerId, amount, reason, null, note).ConfigureAwait(false);
            BalanceChanged?.Invoke(this, new BalanceChangedEventArgs
            {
                ViewerId = viewerId,
                Login = login,
                Balance = newBalance,
                Delta = amount,
                Reason = reason
            });
            return newBalance;
        }

        public async Task<DebitResult> TryDebitAsync(string viewerId, string login, long amount, LedgerReason reason, string effectId, string note = null)
        {
            var result = await _store.TryDebitAsync(viewerId, amount, reason, effectId, note).ConfigureAwait(false);
            if (result.Ok)
            {
                BalanceChanged?.Invoke(this, new BalanceChangedEventArgs
                {
                    ViewerId = viewerId,
                    Login = login,
                    Balance = result.NewBalance,
                    Delta = -amount,
                    Reason = reason
                });
            }
            return result;
        }

        public async Task RefundAsync(string viewerId, string login, long amount, string effectId, string note = null)
        {
            var newBalance = await _store.CreditAsync(viewerId, amount, LedgerReason.ShopRefund, effectId, note).ConfigureAwait(false);
            BalanceChanged?.Invoke(this, new BalanceChangedEventArgs
            {
                ViewerId = viewerId,
                Login = login,
                Balance = newBalance,
                Delta = amount,
                Reason = LedgerReason.ShopRefund
            });
        }

        public Task<IReadOnlyList<LedgerEntry>> GetHistoryAsync(string viewerId, int take = 50)
            => _store.GetHistoryAsync(viewerId, take);

        public Task<IReadOnlyList<LeaderboardRow>> GetLeaderboardAsync(int take = 10)
            => _store.GetLeaderboardAsync(take);
    }
}
