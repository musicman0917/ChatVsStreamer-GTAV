using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Shop;

namespace ChaosMod.Tests.Fakes
{
    public sealed class FakeEffect : IEffect
    {
        public string Id { get; set; } = "test.effect";
        public string ChatCommand { get; set; } = "test";
        public string DisplayName { get; set; } = "Test Effect";
        public string Description { get; set; } = "";
        public EffectCategory Category { get; set; } = EffectCategory.PlayerStatus;
        public EffectTier Tier { get; set; } = EffectTier.Nuisance;
        public int DefaultPointCost { get; set; } = 100;
        public TimeSpan DefaultCooldown { get; set; } = TimeSpan.FromSeconds(30);
        public TimeSpan Duration => TimeSpan.Zero;

        public bool ShouldFailCanExecute { get; set; }
        public bool ShouldFailExecute { get; set; }
        public int ExecuteCallCount { get; private set; }

        public bool CanExecute(IGameContext ctx, out string failureReason)
        {
            failureReason = ShouldFailCanExecute ? "fake failure" : null;
            return !ShouldFailCanExecute;
        }

        public EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ExecuteCallCount++;
            return ShouldFailExecute ? EffectOutcome.Fail("fake execute failure") : EffectOutcome.Ok();
        }
    }

    public sealed class FakeChatResponder : IChatResponder
    {
        public List<string> Messages { get; } = new List<string>();
        public Task SendAsync(string message) { Messages.Add(message); return Task.CompletedTask; }
    }
}
