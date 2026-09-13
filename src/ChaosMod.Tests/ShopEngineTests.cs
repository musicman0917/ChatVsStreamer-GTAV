using System.Threading.Tasks;
using ChaosMod.Core.Ledger;
using ChaosMod.Core.Shop;
using ChaosMod.Tests.Fakes;
using Xunit;

namespace ChaosMod.Tests
{
    public class ShopEngineTests
    {
        private static (ShopEngine shop, PointsLedger ledger, EffectRegistry registry, FakeEffect effect, FakeChatResponder responder, EffectExecutionQueue queue)
            CreateSut(int startingBalance = 1000)
        {
            var store = new FakeLedgerStore();
            var ledger = new PointsLedger(store);
            var registry = new EffectRegistry();
            var effect = new FakeEffect { DefaultPointCost = 100 };
            registry.Register(effect);

            var responder = new FakeChatResponder();
            var queue = new EffectExecutionQueue();
            var shop = new ShopEngine(registry, ledger, new InMemoryCooldownStore(), queue, responder, new ShopOptions());

            ledger.EnsureAccountAsync("v1", "viewer1", "Viewer1").GetAwaiter().GetResult();
            if (startingBalance > 0)
                ledger.CreditAsync("v1", "viewer1", startingBalance, LedgerReason.AdminAdjust).GetAwaiter().GetResult();

            return (shop, ledger, registry, effect, responder, queue);
        }

        [Fact]
        public async Task Purchase_WithSufficientBalance_DebitsAndEnqueues()
        {
            var (shop, ledger, _, _, _, queue) = CreateSut();

            var result = await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");

            Assert.True(result.Accepted);
            Assert.Equal(900, await ledger.GetBalanceAsync("v1"));
            Assert.Equal(1, queue.Count);
        }

        [Fact]
        public async Task Purchase_WithInsufficientBalance_IsRejectedAndNotEnqueued()
        {
            var (shop, ledger, _, _, _, queue) = CreateSut(startingBalance: 50);

            var result = await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");

            Assert.False(result.Accepted);
            Assert.Equal(PurchaseFailureKind.InsufficientBalance, result.FailureKind);
            Assert.Equal(50, await ledger.GetBalanceAsync("v1"));
            Assert.Equal(0, queue.Count);
        }

        [Fact]
        public async Task Purchase_UnknownCommand_IsRejected()
        {
            var (shop, _, _, _, _, _) = CreateSut();

            var result = await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "not-a-real-command");

            Assert.False(result.Accepted);
            Assert.Equal(PurchaseFailureKind.UnknownCommand, result.FailureKind);
        }

        [Fact]
        public async Task Purchase_WhileOnCooldown_IsRejectedAndRefundsNothing()
        {
            var (shop, ledger, _, _, _, _) = CreateSut();

            await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");
            var balanceAfterFirst = await ledger.GetBalanceAsync("v1");

            var second = await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");

            Assert.False(second.Accepted);
            Assert.Equal(PurchaseFailureKind.OnCooldown, second.FailureKind);
            Assert.Equal(balanceAfterFirst, await ledger.GetBalanceAsync("v1"));
        }

        [Fact]
        public async Task Purchase_DisabledEffect_IsRejected()
        {
            var (shop, _, registry, effect, _, _) = CreateSut();
            registry.TryGetById(effect.Id, out var def);
            def.Enabled = false;

            var result = await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");

            Assert.False(result.Accepted);
            Assert.Equal(PurchaseFailureKind.EffectDisabled, result.FailureKind);
        }

        [Fact]
        public async Task DrainAndExecutePending_WhenCanExecuteFails_RefundsPoints()
        {
            var (shop, ledger, _, effect, _, _) = CreateSut();
            effect.ShouldFailCanExecute = true;

            await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");
            var balanceAfterDebit = await ledger.GetBalanceAsync("v1");
            Assert.Equal(900, balanceAfterDebit);

            shop.DrainAndExecutePending(new FakeGameContext());

            // Refund is fire-and-forget (Task, not awaited) inside ShopEngine; give it a tick.
            await Task.Delay(50);

            Assert.Equal(1000, await ledger.GetBalanceAsync("v1"));
            Assert.Equal(0, effect.ExecuteCallCount);
        }

        [Fact]
        public async Task DrainAndExecutePending_WhenExecuteSucceeds_DoesNotRefund()
        {
            var (shop, ledger, _, effect, _, _) = CreateSut();

            await shop.TryPurchaseAsync("v1", "viewer1", "Viewer1", "test");
            shop.DrainAndExecutePending(new FakeGameContext());
            await Task.Delay(50);

            Assert.Equal(900, await ledger.GetBalanceAsync("v1"));
            Assert.Equal(1, effect.ExecuteCallCount);
        }
    }
}
