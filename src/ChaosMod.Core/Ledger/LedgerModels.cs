using System;

namespace ChaosMod.Core.Ledger
{
    public enum LedgerReason
    {
        ChatTick,
        LurkerTick,
        Subscription,
        GiftSub,
        Bits,
        Follow,
        Raid,
        Redemption,
        ShopPurchase,
        ShopRefund,
        AdminAdjust
    }

    public sealed class LedgerEntry
    {
        public long TransactionId { get; set; }
        public string ViewerId { get; set; }
        public long Delta { get; set; }
        public long BalanceAfter { get; set; }
        public LedgerReason Reason { get; set; }
        public string EffectId { get; set; }
        public string Note { get; set; }
        public DateTime CreatedUtc { get; set; }
    }

    public sealed class LeaderboardRow
    {
        public int Rank { get; set; }
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
        public long Balance { get; set; }
    }

    public sealed class DebitResult
    {
        public bool Ok { get; set; }
        public long NewBalance { get; set; }
        public long? TransactionId { get; set; }
    }

    public sealed class BalanceChangedEventArgs : EventArgs
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public long Balance { get; set; }
        public long Delta { get; set; }
        public LedgerReason Reason { get; set; }
    }
}
