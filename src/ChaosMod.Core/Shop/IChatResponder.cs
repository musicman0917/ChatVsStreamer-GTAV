using System.Threading.Tasks;

namespace ChaosMod.Core.Shop
{
    /// <summary>
    /// Lets the shop engine announce purchase/refund outcomes back into chat
    /// without depending on IChatSource directly (a source may be down, or
    /// there may be more than one connected).
    /// </summary>
    public interface IChatResponder
    {
        Task SendAsync(string message);
    }
}
