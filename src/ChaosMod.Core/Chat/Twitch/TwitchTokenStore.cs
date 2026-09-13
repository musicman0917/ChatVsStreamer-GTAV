using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ChaosMod.Core.Chat.Twitch
{
    public enum TwitchTokenRole { Bot, Broadcaster }

    public sealed class TwitchTokenRecord
    {
        [JsonPropertyName("role")]
        public string Role { get; set; }

        [JsonPropertyName("accessToken")]
        public string AccessToken { get; set; }

        [JsonPropertyName("login")]
        public string Login { get; set; }

        [JsonPropertyName("userId")]
        public string UserId { get; set; }

        [JsonPropertyName("scopes")]
        public List<string> Scopes { get; set; } = new List<string>();

        [JsonPropertyName("obtainedAtUtc")]
        public DateTime ObtainedAtUtc { get; set; }
    }

    /// <summary>
    /// Persists Twitch OAuth tokens to ChaosMod/tokens.json, deliberately kept
    /// separate from the human-edited config.ini. Implicit-grant tokens carry
    /// no refresh_token, so this store only ever holds what /oauth2/validate
    /// last confirmed as good; callers must re-run the OAuth flow to replace
    /// an expired/revoked entry.
    /// </summary>
    public sealed class TwitchTokenStore
    {
        private readonly string _path;
        private readonly object _gate = new object();
        private Dictionary<string, TwitchTokenRecord> _byRole;

        public TwitchTokenStore(string path)
        {
            _path = path;
            Load();
        }

        private void Load()
        {
            lock (_gate)
            {
                _byRole = new Dictionary<string, TwitchTokenRecord>(StringComparer.OrdinalIgnoreCase);
                if (!File.Exists(_path)) return;

                try
                {
                    var json = File.ReadAllText(_path);
                    var records = JsonSerializer.Deserialize<List<TwitchTokenRecord>>(json) ?? new List<TwitchTokenRecord>();
                    foreach (var record in records)
                        _byRole[record.Role] = record;
                }
                catch (Exception)
                {
                    // Corrupt/unreadable tokens.json: treat as empty rather than crashing the mod.
                    _byRole = new Dictionary<string, TwitchTokenRecord>(StringComparer.OrdinalIgnoreCase);
                }
            }
        }

        public TwitchTokenRecord Get(TwitchTokenRole role)
        {
            lock (_gate)
            {
                return _byRole.TryGetValue(role.ToString(), out var record) ? record : null;
            }
        }

        public void Save(TwitchTokenRecord record)
        {
            lock (_gate)
            {
                _byRole[record.Role] = record;
                Persist();
            }
        }

        public void Remove(TwitchTokenRole role)
        {
            lock (_gate)
            {
                _byRole.Remove(role.ToString());
                Persist();
            }
        }

        private void Persist()
        {
            var dir = Path.GetDirectoryName(_path);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);

            var json = JsonSerializer.Serialize(new List<TwitchTokenRecord>(_byRole.Values), new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(_path, json);
        }
    }
}
