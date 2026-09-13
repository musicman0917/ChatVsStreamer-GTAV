using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace ChaosMod.Core.Overlay
{
    // DTOs matching the OBS overlay's WebSocket JSON schema (see docs/ARCHITECTURE.md).
    // Kept as plain data classes (no behavior) so OverlayServer can serialize them
    // with System.Text.Json without any special converter configuration.

    public sealed class ShopCommandDto
    {
        [JsonPropertyName("id")] public string Id { get; set; }
        [JsonPropertyName("command")] public string Command { get; set; }
        [JsonPropertyName("displayName")] public string DisplayName { get; set; }
        [JsonPropertyName("category")] public string Category { get; set; }
        [JsonPropertyName("tier")] public string Tier { get; set; }
        [JsonPropertyName("cost")] public int Cost { get; set; }
        [JsonPropertyName("cooldownSeconds")] public int CooldownSeconds { get; set; }
        [JsonPropertyName("enabled")] public bool Enabled { get; set; }
    }

    public sealed class LeaderboardEntryDto
    {
        [JsonPropertyName("rank")] public int Rank { get; set; }
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("balance")] public long Balance { get; set; }
    }

    public sealed class ChatMessageDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "chat_message";
        [JsonPropertyName("ts")] public DateTime Ts { get; set; }
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("displayName")] public string DisplayName { get; set; }
        [JsonPropertyName("message")] public string Message { get; set; }
        [JsonPropertyName("isSub")] public bool IsSub { get; set; }
        [JsonPropertyName("isMod")] public bool IsMod { get; set; }
    }

    public sealed class ShopCatalogDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "shop_catalog";
        [JsonPropertyName("commands")] public List<ShopCommandDto> Commands { get; set; } = new List<ShopCommandDto>();
    }

    public sealed class PurchaseDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "purchase";
        [JsonPropertyName("ts")] public DateTime Ts { get; set; }
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("effectId")] public string EffectId { get; set; }
        [JsonPropertyName("displayName")] public string DisplayName { get; set; }
        [JsonPropertyName("tier")] public string Tier { get; set; }
        [JsonPropertyName("cost")] public int Cost { get; set; }
        [JsonPropertyName("success")] public bool Success { get; set; }
    }

    public sealed class EffectFiredDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "effect_fired";
        [JsonPropertyName("ts")] public DateTime Ts { get; set; }
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("effectId")] public string EffectId { get; set; }
        [JsonPropertyName("displayName")] public string DisplayName { get; set; }
        [JsonPropertyName("tier")] public string Tier { get; set; }
        [JsonPropertyName("forcedByHotkey")] public bool ForcedByHotkey { get; set; }
    }

    public sealed class RefundDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "refund";
        [JsonPropertyName("ts")] public DateTime Ts { get; set; }
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("amount")] public int Amount { get; set; }
        [JsonPropertyName("effectId")] public string EffectId { get; set; }
        [JsonPropertyName("reason")] public string Reason { get; set; }
    }

    public sealed class BalanceUpdateDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "balance_update";
        [JsonPropertyName("viewerLogin")] public string ViewerLogin { get; set; }
        [JsonPropertyName("balance")] public long Balance { get; set; }
    }

    public sealed class LeaderboardDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "leaderboard";
        [JsonPropertyName("entries")] public List<LeaderboardEntryDto> Entries { get; set; } = new List<LeaderboardEntryDto>();
    }

    public sealed class ClipCreatedDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "clip_created";
        [JsonPropertyName("effectId")] public string EffectId { get; set; }
        [JsonPropertyName("url")] public string Url { get; set; }
    }

    public sealed class SnapshotDto
    {
        [JsonPropertyName("type")] public string Type { get; } = "snapshot";
        [JsonPropertyName("catalog")] public List<ShopCommandDto> Catalog { get; set; } = new List<ShopCommandDto>();
        [JsonPropertyName("leaderboard")] public List<LeaderboardEntryDto> Leaderboard { get; set; } = new List<LeaderboardEntryDto>();
        [JsonPropertyName("recentChat")] public List<ChatMessageDto> RecentChat { get; set; } = new List<ChatMessageDto>();
    }
}
