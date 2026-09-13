using System.Collections.Generic;
using System.Threading.Tasks;

namespace ChaosMod.Core.Ledger
{
    /// <summary>
    /// Storage seam for the points ledger. SqliteLedgerStore is the real
    /// implementation; a FakeLedgerStore (ChaosMod.Tests) backs unit tests
    /// without touching disk.
    /// </summary>
    public interface ILedgerStore
    {
        void Initialize();

        Task EnsureAccountAsync(string viewerId, string login, string displayName);

        Task<long> GetBalanceAsync(string viewerId);

        Task<long> CreditAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note);

        /// <summary>
        /// Atomic conditional debit: only succeeds if the viewer's balance is
        /// currently >= amount. Must be implemented as a single conditional
        /// UPDATE (checked for exactly one affected row), never read-then-write,
        /// since purchase attempts for the same viewer can race across threads.
        /// </summary>
        Task<DebitResult> TryDebitAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note);

        Task<IReadOnlyList<LedgerEntry>> GetHistoryAsync(string viewerId, int take);

        Task<IReadOnlyList<LeaderboardRow>> GetLeaderboardAsync(int take);
    }
}
