using System.Threading.Tasks;
using ChaosMod.Core.Ledger;
using ChaosMod.Tests.Fakes;
using Xunit;

namespace ChaosMod.Tests
{
    public class PointsLedgerTests
    {
        private static PointsLedger CreateSut() => new PointsLedger(new FakeLedgerStore());

        [Fact]
        public async Task CreditAsync_IncreasesBalance()
        {
            var ledger = CreateSut();
            await ledger.EnsureAccountAsync("v1", "viewer1", "Viewer1");

            await ledger.CreditAsync("v1", "viewer1", 250, LedgerReason.ChatTick);

            Assert.Equal(250, await ledger.GetBalanceAsync("v1"));
        }

        [Fact]
        public async Task TryDebitAsync_WithSufficientBalance_Succeeds()
        {
            var ledger = CreateSut();
            await ledger.EnsureAccountAsync("v1", "viewer1", "Viewer1");
            await ledger.CreditAsync("v1", "viewer1", 500, LedgerReason.ChatTick);

            var result = await ledger.TryDebitAsync("v1", "viewer1", 200, LedgerReason.ShopPurchase, "some.effect");

            Assert.True(result.Ok);
            Assert.Equal(300, result.NewBalance);
        }

        [Fact]
        public async Task TryDebitAsync_WithInsufficientBalance_Fails()
        {
            var ledger = CreateSut();
            await ledger.EnsureAccountAsync("v1", "viewer1", "Viewer1");
            await ledger.CreditAsync("v1", "viewer1", 50, LedgerReason.ChatTick);

            var result = await ledger.TryDebitAsync("v1", "viewer1", 200, LedgerReason.ShopPurchase, "some.effect");

            Assert.False(result.Ok);
            Assert.Equal(50, await ledger.GetBalanceAsync("v1"));
        }

        [Fact]
        public async Task RefundAsync_RestoresBalance()
        {
            var ledger = CreateSut();
            await ledger.EnsureAccountAsync("v1", "viewer1", "Viewer1");
            await ledger.CreditAsync("v1", "viewer1", 500, LedgerReason.ChatTick);
            await ledger.TryDebitAsync("v1", "viewer1", 300, LedgerReason.ShopPurchase, "some.effect");

            await ledger.RefundAsync("v1", "viewer1", 300, "some.effect");

            Assert.Equal(500, await ledger.GetBalanceAsync("v1"));
        }
    }
}
