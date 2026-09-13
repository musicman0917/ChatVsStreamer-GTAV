using System;
using System.Threading;
using System.Threading.Tasks;
using ChaosMod.Core.Abstractions;
using TwitchLib.Client;
using TwitchLib.Client.Events;
using TwitchLib.Client.Models;
using TwitchLib.Communication.Clients;
using TwitchLib.Communication.Models;

namespace ChaosMod.Core.Chat.Twitch
{
    /// <summary>
    /// IChatSource implementation over TwitchLib.Client (IRC-based chat).
    /// All TwitchLib events fire on TwitchLib's own background thread — this
    /// class only re-raises them as ChatMessageReceivedEventArgs; it never
    /// touches the game directly, keeping it safe to live in ChaosMod.Core.
    /// </summary>
    public sealed class TwitchChatSource : IChatSource, IDisposable
    {
        private readonly string _botLogin;
        private readonly string _oauthToken;
        private readonly string _channelName;
        private TwitchClient _client;

        public string SourceName => "Twitch";
        public bool IsConnected => _client?.IsConnected ?? false;

        public event EventHandler<ChatMessageReceivedEventArgs> MessageReceived;

        public TwitchChatSource(string botLogin, string oauthToken, string channelName)
        {
            _botLogin = botLogin;
            _oauthToken = oauthToken;
            _channelName = channelName;
        }

        public Task ConnectAsync(CancellationToken ct)
        {
            var credentials = new ConnectionCredentials(_botLogin, _oauthToken);
            var clientOptions = new ClientOptions
            {
                MessagesAllowedInPeriod = 750,
                ThrottlingPeriod = TimeSpan.FromSeconds(30)
            };
            var socketClient = new WebSocketClient(clientOptions);
            _client = new TwitchClient(socketClient);
            _client.Initialize(credentials, _channelName);

            _client.OnMessageReceived += OnMessageReceived;
            _client.OnConnected += (s, e) => { };
            _client.OnError += (s, e) => System.Diagnostics.Debug.WriteLine($"[ChaosMod] Twitch chat error: {e.Exception}");

            _client.Connect();
            return Task.CompletedTask;
        }

        private void OnMessageReceived(object sender, OnMessageReceivedArgs e)
        {
            var msg = e.ChatMessage;
            MessageReceived?.Invoke(this, new ChatMessageReceivedEventArgs
            {
                ViewerId = msg.UserId,
                Login = msg.Username,
                DisplayName = msg.DisplayName,
                Message = msg.Message,
                IsSubscriber = msg.IsSubscriber,
                IsModerator = msg.IsModerator,
                IsBroadcaster = msg.IsBroadcaster,
                ReceivedAtUtc = DateTime.UtcNow
            });
        }

        public Task DisconnectAsync()
        {
            _client?.Disconnect();
            return Task.CompletedTask;
        }

        public Task SendMessageAsync(string message)
        {
            if (_client != null && _client.IsConnected)
                _client.SendMessage(_channelName, message);
            return Task.CompletedTask;
        }

        public void Dispose()
        {
            try { _client?.Disconnect(); } catch { /* ignore */ }
        }
    }
}
