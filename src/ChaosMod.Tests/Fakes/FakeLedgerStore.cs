using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using ChaosMod.Core.Ledger;

namespace ChaosMod.Tests.Fakes
{
    /// <summary>In-memory ILedgerStore for unit tests — no disk I/O, no SQLite.</summary>
    public sealed class FakeLedgerStore : ILedgerStore
    {
        private readonly Dictionary<string, long> _balances = new Dictionary<string, long>();
        private readonly List<LedgerEntry> _entries = new List<LedgerEntry>();
        private long _nextTransactionId = 1;
        private readonly object _gate = new object();

        public void Initialize() { }

        public Task EnsureAccountAsync(string viewerId, string login, string displayName)
        {
            lock (_gate)
            {
                if (!_balances.ContainsKey(viewerId)) _balances[viewerId] = 0;
            }
            return Task.CompletedTask;
        }

        public Task<long> GetBalanceAsync(string viewerId)
        {
            lock (_gate) return Task.FromResult(_balances.TryGetValue(viewerId, out var b) ? b : 0);
        }

        public Task<long> CreditAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note)
        {
            lock (_gate)
            {
                _balances.TryGetValue(viewerId, out var current);
                var newBalance = current + amount;
                _balances[viewerId] = newBalance;
                _entries.Add(new LedgerEntry { TransactionId = _nextTransactionId++, ViewerId = viewerId, Delta = amount, BalanceAfter = newBalance, Reason = reason, EffectId = effectId, Note = note });
                return Task.FromResult(newBalance);
            }
        }

        public Task<DebitResult> TryDebitAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note)
        {
            lock (_gate)
            {
                _balances.TryGetValue(viewerId, out var current);
                if (current < amount)
                    return Task.FromResult(new DebitResult { Ok = false, NewBalance = current, TransactionId = null });

                var newBalance = current - amount;
                _balances[viewerId] = newBalance;
                var txId = _nextTransactionId++;
                _entries.Add(new LedgerEntry { TransactionId = txId, ViewerId = viewerId, Delta = -amount, BalanceAfter = newBalance, Reason = reason, EffectId = effectId, Note = note });
                return Task.FromResult(new DebitResult { Ok = true, NewBalance = newBalance, TransactionId = txId });
            }
        }

        public Task<IReadOnlyList<LedgerEntry>> GetHistoryAsync(string viewerId, int take)
        {
            lock (_gate)
                return Task.FromResult((IReadOnlyList<LedgerEntry>)_entries.Where(e => e.ViewerId == viewerId).OrderByDescending(e => e.TransactionId).Take(take).ToList());
        }

        public Task<IReadOnlyList<LeaderboardRow>> GetLeaderboardAsync(int take)
        {
            lock (_gate)
            {
                var rows = _balances.OrderByDescending(kv => kv.Value).Take(take)
                    .Select((kv, i) => new LeaderboardRow { Rank = i + 1, ViewerId = kv.Key, Login = kv.Key, DisplayName = kv.Key, Balance = kv.Value })
                    .ToList();
                return Task.FromResult((IReadOnlyList<LeaderboardRow>)rows);
            }
        }
    }
}
