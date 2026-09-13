using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using ChaosMod.Core.Shop;

namespace ChaosMod.Core.Chat
{
    /// <summary>Adapts whatever IChatSource is currently active into the ShopEngine's IChatResponder seam.</summary>
    public sealed class ChatSourceResponder : IChatResponder
    {
        public IChatSource ActiveSource { get; set; }

        public Task SendAsync(string message)
        {
            return ActiveSource != null && ActiveSource.IsConnected
                ? ActiveSource.SendMessageAsync(message)
                : Task.CompletedTask;
        }
    }
}
