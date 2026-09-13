namespace ChaosMod.Core.Abstractions
{
    /// <summary>Minimal identity needed to credit a currently-connected viewer who hasn't necessarily typed anything (the "lurker trickle").</summary>
    public sealed class ActiveChatterInfo
    {
        public string ViewerId { get; set; }
        public string Login { get; set; }
        public string DisplayName { get; set; }
    }
}
