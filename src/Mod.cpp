#include "Mod.h"
#include "Log.h"
#include "Util.h"

#include <fstream>
#include <random>
#include <sstream>

namespace cvs
{
    using namespace nat;

    namespace
    {
        constexpr double kQueueTimeoutSec = 120.0;  // refund if the game can't run it by then
        constexpr double kGameAliveSec = 5.0;       // no script tick for this long = game isn't running
        constexpr double kLedgerSaveSec = 30.0;
        constexpr double kSettingsCheckSec = 2.0;
        constexpr double kPingSec = 15.0;

        std::filesystem::path HiddenPath() { return DataDir() / L"leaderboard_hidden.txt"; }

        // Common chat bots without "bot" in their names, which would be left off anyway.
        constexpr const char* kHiddenTemplate =
            "# Accounts left off the overlay's leaderboard, one Twitch name per line. They keep their points.\n"
            "# Your channel, the mod's own bot account and any name with \"bot\" in it are always left off.\n"
            "# Twitch lists your channel's bots under Chat Bots, in the community list beside chat.\n"
            "# Changes apply within a couple of seconds.\n"
            "streamelements\n"
            "streamlabs\n"
            "streamstickers\n"
            "soundalerts\n"
            "pokemoncommunitygame\n"
            "frostytools\n"
            "limelightstream\n";

        std::filesystem::path RewardsPath() { return DataDir() / L"channel_point_rewards.txt"; }

        // The same three rewards and amounts as the Stardew Valley version.
        constexpr const char* kRewardsTemplate =
            "# Channel point rewards that give chaos points, one per line: the reward's title on Twitch, then = and the points.\n"
            "# The title has to match the reward on Twitch, though capitals don't matter. Other rewards are left alone.\n"
            "# Needs BroadcasterToken in ChatVsStreamer.ini: a token for your channel's own account with channel:read:redemptions.\n"
            "# Changes apply within a couple of seconds.\n"
            "Get Chaos Points (Small) = 50\n"
            "Get Chaos Points (Medium) = 150\n"
            "Get Chaos Points (Large) = 400\n";

        std::map<std::string, std::string> ParseForm(const std::string& body)
        {
            auto decode = [](const std::string& s)
            {
                std::string out;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    if (s[i] == '+') out.push_back(' ');
                    else if (s[i] == '%' && i + 2 < s.size())
                    {
                        out.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
                        i += 2;
                    }
                    else out.push_back(s[i]);
                }
                return out;
            };
            std::map<std::string, std::string> form;
            for (auto& pair : Split(body, '&'))
            {
                size_t eq = pair.find('=');
                if (eq != std::string::npos) form[decode(pair.substr(0, eq))] = decode(pair.substr(eq + 1));
            }
            return form;
        }

        std::string StripLineBreaks(std::string s)
        {
            s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }), s.end());
            return s;
        }

        HttpReply JsonOk() { return { 200, "application/json", R"({"ok":true})" }; }
        HttpReply JsonError(int status, const std::string& message)
        {
            return { status, "application/json", std::format(R"({{"ok":false,"error":"{}"}})", JsonEscape(message)) };
        }
    }

    void Mod::Start()
    {
        log::Init();
        log::Info("Chat vs Streamer for GTA V {} starting. Data folder: {}", kVersion, DataDir().string());
#ifdef CVS_BACKEND_RPH
        log::Info("Running through RAGE Plugin Hook on game version {}", hostGameVersion);
#else
        log::Info("Script Hook V reports game version {}", (int)getGameVersion());
#endif

        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        m_boot = BootConfig::Load();
        m_settings.LoadOrCreate();
        m_economy = std::make_unique<Economy>(m_settings);
        m_economy->Load();
        ReloadHiddenIfChanged();

        m_web.onRequest = [this](const HttpRequest& r) { return HandleHttp(r); };
        m_web.Start(m_boot.webPort);

        // Set even without Twitch credentials: the panel's chat simulator uses them.
        m_chat.onMessage = [this](const ChatMessage& m) { OnChatMessage(m); };
        m_chat.onEvent = [this](const ChatEvent& e) { OnChatEvent(e); };
        if (m_boot.TwitchConfigured()) m_chat.Start(m_boot.channel, m_boot.botUsername, m_boot.oauthToken);
        else log::Error("Twitch isn't configured. Edit {} and restart the game.", (DataDir() / L"ChatVsStreamer.ini").string());

        ReloadRewardsIfChanged();
        m_eventSub.onRedemption = [this](const Redemption& r) { OnRedemption(r); };
        if (!m_boot.channel.empty() && !m_boot.broadcasterToken.empty()) m_eventSub.Start(m_boot.channel, m_boot.broadcasterToken);
        else log::Info("Channel point rewards are off. To have them give chaos points, add BroadcasterToken to ChatVsStreamer.ini (see the README).");

        m_lastActivity = NowSeconds();
        m_running = true;
        m_worker = std::thread(&Mod::WorkerLoop, this);
        m_started = true;
    }

    void Mod::Stop()
    {
        m_running = false;
        m_outcomeCv.notify_all();
        if (m_worker.joinable()) m_worker.join();
        m_eventSub.Stop();
        m_chat.Stop();
        m_web.Stop();
        if (m_economy) m_economy->SaveIfDirty();
        WSACleanup();
    }

    bool Mod::GameAlive() const
    {
        return NowSeconds() - m_lastTick.load() < kGameAliveSec;
    }

    // ── Chat ────────────────────────────────────────────────────────────────────

    void Mod::Reply(bool simulated, const std::string& text)
    {
        // Replies to simulated chat stay on the panel. A test must never post to real chat.
        if (simulated) m_web.Broadcast(std::format(R"({{"type":"reply","text":"{}"}})", JsonEscape(text)));
        else m_chat.Say(text);
    }

    // Called on the EventSub thread, or a web thread for the panel and the tests.
    void Mod::OnRedemption(const Redemption& r)
    {
        long long points = 0;
        {
            std::lock_guard lock(m_rewardsMutex);
            auto it = m_rewards.find(Lower(Trim(r.rewardTitle)));
            if (it != m_rewards.end()) points = it->second;
        }
        if (points <= 0) return; // some other reward, such as one Streamer.bot handles

        m_economy->Add(r.login, r.display, points);
        log::Info("{} redeemed {} for {} points", r.display, r.rewardTitle, points);
        Alert("points", r.display, std::format("redeemed {} (+{} pts)", r.rewardTitle, points));
        // Worded like the Stardew Valley version's reply.
        Reply(r.simulated, std::format("✨ @{} redeemed {} and received {} chaos points! (balance: {}pts)",
                                       r.display, r.rewardTitle, points, m_economy->Balance(r.login)));
    }

    void Mod::OnChatMessage(const ChatMessage& msg)
    {
        m_economy->OnChat(msg.login, msg.display, msg.subTier);

        if (msg.bits > 0)
        {
            long long pts = (long long)msg.bits * m_settings.Get("PointsPerBit");
            m_economy->Add(msg.login, msg.display, pts);
            Alert("bits", msg.display, std::format("cheered {} bits (+{} pts)", msg.bits, pts));
        }

        auto text = Trim(msg.text);
        if (text.empty() || text[0] != '!') return;
        auto words = Split(text, ' ');
        std::vector<std::string> args;
        for (size_t i = 1; i < words.size(); ++i)
            if (!words[i].empty()) args.push_back(words[i]);
        HandleCommand(msg, Lower(words[0]), args);
    }

    void Mod::OnChatEvent(const ChatEvent& ev)
    {
        if (ev.type == "sub" || ev.type == "resub")
        {
            m_economy->Add(ev.login, ev.display, m_settings.Get("SubBonus"));
            Alert("sub", ev.display, std::format("subscribed (+{} pts)", m_settings.Get("SubBonus")));
        }
        else if (ev.type == "submysterygift")
        {
            // Twitch sends one subgift per recipient after this, and those carry the points.
            Alert("sub", ev.display, std::format("gifted {} subs!", ev.count));
        }
        else if (ev.type == "subgift")
        {
            m_economy->Add(ev.login, ev.display, m_settings.Get("GiftBonus"));
        }
        else if (ev.type == "raid")
        {
            long long pts = (long long)ev.count * m_settings.Get("RaidPerViewer");
            m_economy->Add(ev.login, ev.display, pts);
            Alert("raid", ev.display, std::format("raided with {} viewers (+{} pts)", ev.count, pts));

            if (m_settings.Get("RaidAttack") && ev.count >= m_settings.Get("RaidAttackMinViewers") && m_settings.Get("Enabled"))
            {
                Purchase p;
                p.effectId = "mob";
                p.display = ev.display;
                p.count = std::max(1, ev.count / 5);
                p.force = true;
                p.simulated = ev.simulated;
                Enqueue(std::move(p));
            }
        }
    }

    void Mod::HandleCommand(const ChatMessage& msg, const std::string& cmd, const std::vector<std::string>& args)
    {
        if (cmd == "!balance" || cmd == "!points")
            Reply(msg.simulated, std::format("@{} you have {} chaos points.", msg.display, m_economy->Balance(msg.login)));
        else if (cmd == "!shop") CmdShop(msg, args);
        else if (cmd == "!info" && !args.empty()) CmdInfo(msg, args[0]);
        else if (cmd == "!buy")
        {
            if (args.empty()) Reply(msg.simulated, std::format("@{} usage: !buy <effect>. See !shop", msg.display));
            else CmdBuy(msg, Lower(args[0]));
        }
        else if (cmd == "!give") CmdGive(msg, args);
        else if (cmd == "!cvs" && msg.isMod) CmdControl(msg, args);
        else if (const EffectDef* e = FindDirectCommand(cmd.substr(1))) CmdBuy(msg, e->id);
    }

    // With 50+ effects a full list is several chat lines, so !shop alone gives the
    // tiers and !shop <tier> gives one tier.
    void Mod::CmdShop(const ChatMessage& msg, const std::vector<std::string>& args)
    {
        static constexpr Tier kTiers[] = { Tier::Nuisance, Tier::Disruptive, Tier::Painful, Tier::Devastating, Tier::Blessing };
        const std::string wanted = args.empty() ? "" : Lower(args[0]);

        if (wanted.empty())
        {
            Reply(msg.simulated, "Spend chaos points with !buy <name>. See a tier with: !shop nuisance, !shop disruptive, "
                                 "!shop painful, !shop devastating, !shop blessing. Details: !info <name>");
            return;
        }

        for (Tier tier : kTiers)
        {
            if (wanted != Lower(TierName(tier))) continue;
            std::string line = std::string(TierName(tier)) + ": ";
            for (auto& e : AllEffects())
            {
                if (e.tier != tier || !m_settings.EffectEnabled(e.id)) continue;
                std::string item = std::format("{} {} | ", e.id, m_settings.Price(e.id, e.defaultPrice));
                if (line.size() + item.size() > 480) { Reply(msg.simulated, line); line.clear(); }
                line += item;
            }
            if (line.size() >= 3) line.resize(line.size() - 3);
            Reply(msg.simulated, line);
            return;
        }
        Reply(msg.simulated, std::format("@{} tiers are nuisance, disruptive, painful, devastating and blessing.", msg.display));
    }

    void Mod::CmdInfo(const ChatMessage& msg, const std::string& effectId)
    {
        const EffectDef* e = FindEffect(effectId);
        if (!e) { Reply(msg.simulated, std::format("@{} no effect called '{}'. Try !shop", msg.display, FeedSafe(effectId))); return; }
        Reply(msg.simulated, std::format("{} ({}, {} pts): {}", e->name, TierName(e->tier),
                                         m_settings.Price(e->id, e->defaultPrice), e->description));
    }

    void Mod::CmdBuy(const ChatMessage& msg, const std::string& effectId)
    {
        if (!m_settings.Get("Enabled")) { Reply(msg.simulated, "Chat vs Streamer is paused right now."); return; }

        const EffectDef* e = FindEffect(effectId);
        if (!e) { Reply(msg.simulated, std::format("@{} no effect called '{}'. Try !shop", msg.display, FeedSafe(effectId))); return; }
        if (!m_settings.EffectEnabled(e->id)) { Reply(msg.simulated, std::format("@{} {} is switched off.", msg.display, e->name)); return; }
        if (!GameAlive()) { Reply(msg.simulated, "The game isn't running right now, so try again in a moment."); return; }

        long long price = m_settings.Price(e->id, e->defaultPrice);
        if (!m_economy->TrySpend(msg.login, price))
        {
            Reply(msg.simulated, std::format("@{} {} costs {} and you have {}.", msg.display, e->name, price, m_economy->Balance(msg.login)));
            return;
        }

        Purchase p;
        p.effectId = e->id;
        p.login = msg.login;
        p.display = msg.display;
        p.paid = price;
        p.simulated = msg.simulated;

        size_t position = 0;
        if (!Enqueue(std::move(p), &position))
        {
            m_economy->Add(msg.login, msg.display, price);
            Reply(msg.simulated, std::format("@{} the queue is full, so nothing was charged. Try again shortly.", msg.display));
            return;
        }
        if (position > 1 && !(e->flags & Silent))
            Reply(msg.simulated, std::format("@{} bought {}. You're #{} in the queue.", msg.display, e->name, position));
    }

    void Mod::CmdGive(const ChatMessage& msg, const std::vector<std::string>& args)
    {
        if (args.size() < 2) return;
        std::string target = Lower(args[0]);
        if (StartsWith(target, "@")) target.erase(0, 1);
        if (!IsValidLogin(target)) { Reply(msg.simulated, std::format("@{} that isn't a valid username.", msg.display)); return; }
        long long amount = ToInt(args[1], 0);
        if (amount <= 0) return;

        // Mods mint points; everyone else transfers from their own balance.
        if (!msg.isMod && !m_economy->TrySpend(msg.login, amount))
        {
            Reply(msg.simulated, std::format("@{} you don't have {} points to give.", msg.display, amount));
            return;
        }
        m_economy->Add(target, "", amount);
        Reply(msg.simulated, std::format("@{} gave {} points to {}.", msg.display, amount, target));
    }

    void Mod::CmdControl(const ChatMessage& msg, const std::vector<std::string>& args)
    {
        const std::string action = args.empty() ? "status" : Lower(args[0]);
        if (action == "on") { SetEnabled(true); Reply(msg.simulated, "Chat vs Streamer is on."); }
        else if (action == "off") { SetEnabled(false); Reply(msg.simulated, "Chat vs Streamer is paused."); }
        else if (action == "clear") Reply(msg.simulated, std::format("Cleared {} queued effects and refunded them.", ClearQueue()));
        else if (action == "status")
        {
            size_t queued;
            {
                std::lock_guard lock(m_queueMutex);
                queued = m_queue.size();
            }
            Reply(msg.simulated, std::format("Chat vs Streamer: {}. Game: {}. {} queued.", m_settings.Get("Enabled") ? "on" : "paused",
                                             GameAlive() ? BlockReason((Block)m_block.load()) : "not running", queued));
        }
    }

    // ── Queue ───────────────────────────────────────────────────────────────────

    bool Mod::Enqueue(Purchase purchase, size_t* positionOut)
    {
        purchase.queuedAt = NowSeconds();
        std::lock_guard lock(m_queueMutex);
        if ((int)m_queue.size() >= m_settings.Get("QueueLimit")) return false;
        m_queue.push_back(std::move(purchase));
        if (positionOut) *positionOut = m_queue.size();
        m_lastActivity = NowSeconds();
        return true;
    }

    size_t Mod::ClearQueue()
    {
        std::deque<Purchase> removed;
        {
            std::lock_guard lock(m_queueMutex);
            removed.swap(m_queue);
        }
        for (auto& p : removed)
            if (p.paid > 0) m_economy->Add(p.login, p.display, p.paid);
        return removed.size();
    }

    bool Mod::FireChaosGods()
    {
        const int maxTier = m_settings.Get("ChaosGodsPainful") ? (int)Tier::Painful : (int)Tier::Disruptive;
        // Only what could run right now. Anything else waits in the queue and is dropped after
        // two minutes, which looked like F7 doing nothing.
        const bool inVehicle = m_inVehicle, onMission = m_onMission;
        const bool holdRisky = m_settings.Get("BlockRiskyInMissions") != 0;
        const std::vector<ActiveEffect> running = m_engine.Snapshot();
        const auto isRunning = [&](const std::string& id)
        {
            return std::any_of(running.begin(), running.end(), [&](const ActiveEffect& a) { return a.id == id; });
        };
        std::vector<const EffectDef*> pool;
        for (auto& e : AllEffects())
        {
            if ((int)e.tier > maxTier || !m_settings.EffectEnabled(e.id) || isRunning(e.id)) continue;
            if ((e.flags & NeedsVehicle) && !inVehicle) continue;
            if ((e.flags & NeedsOnFoot) && inVehicle) continue;
            if (holdRisky && (e.flags & RiskyInMission) && onMission) continue;
            pool.push_back(&e);
        }
        if (pool.empty()) return false;

        static std::mt19937 rng{ std::random_device{}() };
        const EffectDef* pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];

        Purchase p;
        p.effectId = pick->id;
        p.display = "The Chaos Gods";
        p.force = true;
        return Enqueue(std::move(p));
    }

    void Mod::SetEnabled(bool enabled)
    {
        m_settings.Set("Enabled", enabled ? 1 : 0);
        Alert("status", "Chat vs Streamer", enabled ? "is back on" : "is paused");
    }

    // ── Script fiber ────────────────────────────────────────────────────────────

    void Mod::OnScriptStart()
    {
        m_engine.OnScriptStart();
        m_lastFire = 0;
    }

    void Mod::OnKey(DWORD key, bool wasDown, bool isUp)
    {
        if (!m_started || wasDown || isUp) return;
        if ((int)key == m_settings.Get("HotkeyChaos")) m_hotkeyChaos = true;
        if ((int)key == m_settings.Get("HotkeyPause")) m_hotkeyPause = true;
    }

    void Mod::PollHotkeys(bool (*isDown)(int virtualKey))
    {
        if (!m_started) return;
        auto poll = [&](const char* setting, bool& wasDown, std::atomic<bool>& pressed)
        {
            const bool down = isDown(m_settings.Get(setting));
            if (down && !wasDown) pressed = true;
            wasDown = down;
        };
        poll("HotkeyChaos", m_chaosKeyWasDown, m_hotkeyChaos);
        poll("HotkeyPause", m_pauseKeyWasDown, m_hotkeyPause);
    }

    void Mod::HandleHotkeys()
    {
        if (m_hotkeyPause.exchange(false))
        {
            // Saving settings touches the disk, so the worker thread applies it.
            const bool enable = !m_settings.Get("Enabled");
            m_pendingEnabled = enable ? 1 : 0;
            m_outcomeCv.notify_one();
            PostFeed(enable ? "~g~Chat vs Streamer on" : "~r~Chat vs Streamer paused");
        }
        if (m_hotkeyChaos.exchange(false)) FireChaosGods();
    }

    void Mod::ScriptTick()
    {
        if (!m_started) return;

        const double now = NowSeconds();
        m_lastTick = now;
        const EffectEngine::Status status = m_engine.Update(now);
        m_block = (int)status.block;
        m_inVehicle = status.inVehicle;
        m_onMission = status.onMission;

        HandleHotkeys();

        if (now - m_lastChaosCheck >= 1.0)
        {
            m_lastChaosCheck = now;
            if (m_settings.Get("ChaosGods") && m_settings.Get("Enabled") && status.CanAct() &&
                now - m_lastActivity.load() >= m_settings.Get("ChaosGodsQuietMinutes") * 60.0)
                FireChaosGods();
        }

        ServiceQueue(now, status);
    }

    void Mod::ServiceQueue(double now, const EffectEngine::Status& status)
    {
        Purchase next;
        const EffectDef* def = nullptr;
        {
            std::lock_guard lock(m_queueMutex);
            if (m_queue.empty()) return;

            // Anything too old is refunded rather than fired late. This is also what stops
            // a backlog from going off all at once after a pause or an alt-tab.
            for (auto it = m_queue.begin(); it != m_queue.end();)
            {
                if (now - it->queuedAt < kQueueTimeoutSec) { ++it; continue; }
                std::string reason = it->waitReason.empty() ? "the game stayed busy too long" : it->waitReason;
                {
                    std::lock_guard outcomeLock(m_outcomeMutex);
                    m_outcomes.push_back({ std::move(*it), EffectResult::Kind::Fail, std::move(reason) });
                }
                m_outcomeCv.notify_one();
                it = m_queue.erase(it);
            }

            if (!status.CanAct())
            {
                for (auto& p : m_queue) p.waitReason = BlockReason(status.block);
                return;
            }
            const bool enabled = m_settings.Get("Enabled") != 0;
            if (!enabled)
                for (auto& p : m_queue)
                    if (!p.force) p.waitReason = "the streamer paused chat effects";
            if (now - m_lastFire < m_settings.Get("MinSpacing")) return;

            const int cooldown = m_settings.Get("EffectCooldown");
            auto it = std::find_if(m_queue.begin(), m_queue.end(), [&](const Purchase& p)
            {
                if (now < p.nextAttempt) return false;
                if (p.force) return true; // the streamer's own buttons work even while chat is paused
                if (!enabled) return false;
                auto last = m_lastFired.find(p.effectId);
                return last == m_lastFired.end() || now - last->second >= cooldown;
            });
            if (it == m_queue.end()) return;
            next = std::move(*it);
            m_queue.erase(it);
        }

        def = FindEffect(next.effectId);
        if (!def || (!next.force && !m_settings.EffectEnabled(def->id)))
        {
            {
                std::lock_guard lock(m_outcomeMutex);
                m_outcomes.push_back({ std::move(next), EffectResult::Kind::Fail, "it was switched off" });
            }
            m_outcomeCv.notify_one();
            return;
        }

        EffectOptions options;
        options.blockRisky = m_settings.Get("BlockRiskyInMissions") != 0;
        options.ejectParachute = m_settings.Get("EjectParachute") != 0;
        options.abductParachute = m_settings.Get("AbductParachute") != 0;
        options.fedsClearSeconds = m_settings.Get("FedsClearSeconds");
        EffectResult result = m_engine.Run(*def, next.display, next.count, options);

        if (result.kind == EffectResult::Kind::Retry)
        {
            next.nextAttempt = now + result.retryAfter;
            next.waitReason = std::move(result.reason);
            std::lock_guard lock(m_queueMutex);
            m_queue.push_front(std::move(next));
            return;
        }

        if (result.kind == EffectResult::Kind::Ok)
        {
            m_lastFire = now;
            m_lastFired[def->id] = now;
            if (!(def->flags & Silent) && m_settings.Get("Alerts"))
            {
                const char* color = def->tier == Tier::Blessing ? "~g~" : "~r~";
                PostFeed(std::format("~b~{}~s~: {}{}", FeedSafe(next.display), color, def->name));
            }
        }

        {
            std::lock_guard lock(m_outcomeMutex);
            m_outcomes.push_back({ std::move(next), result.kind, std::move(result.reason) });
        }
        m_outcomeCv.notify_one();
    }

    void Mod::PostFeed(const std::string& text)
    {
        BEGIN_TEXT_COMMAND_THEFEED_POST("STRING");
        ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
        END_TEXT_COMMAND_THEFEED_POST_TICKER(false, false);
    }

    // ── Worker thread ───────────────────────────────────────────────────────────

    void Mod::WorkerLoop()
    {
        double lastSave = NowSeconds(), lastPassive = NowSeconds(), lastPing = 0, lastSettings = NowSeconds();

        while (m_running)
        {
            log::Guard("The worker thread", [&]
            {
                std::deque<Outcome> outcomes;
                {
                    std::unique_lock lock(m_outcomeMutex);
                    m_outcomeCv.wait_for(lock, std::chrono::milliseconds(200), [this]
                    {
                        return !m_running || !m_outcomes.empty() || m_pendingEnabled.load() >= 0;
                    });
                    outcomes.swap(m_outcomes);
                }
                for (auto& o : outcomes) HandleOutcome(o);

                const int pendingEnabled = m_pendingEnabled.exchange(-1);
                if (pendingEnabled >= 0) SetEnabled(pendingEnabled != 0);

                const double now = NowSeconds();
                if (now - lastSettings >= kSettingsCheckSec)
                {
                    lastSettings = now;
                    m_settings.ReloadIfChanged();
                    ReloadHiddenIfChanged();
                    ReloadRewardsIfChanged();
                }
                if (now - lastPassive >= m_settings.Get("PassiveMinutes") * 60.0) { lastPassive = now; m_economy->PassiveTick(); }
                if (now - lastSave >= kLedgerSaveSec) { lastSave = now; m_economy->SaveIfDirty(); }
                if (now - lastPing >= kPingSec) { lastPing = now; m_web.Broadcast(R"({"type":"ping"})"); }
            });
        }
    }

    void Mod::HandleOutcome(const Outcome& o)
    {
        const Purchase& p = o.purchase;
        const EffectDef* def = FindEffect(p.effectId);
        const std::string name = def ? def->name : p.effectId;

        if (o.kind == EffectResult::Kind::Ok)
        {
            log::Info("{} -> {}", p.display, p.effectId);
            m_web.Broadcast(std::format(R"({{"type":"fired","effect":"{}"}})", JsonEscape(p.effectId)));
            if (def && !(def->flags & Silent))
                Alert(def->tier == Tier::Blessing ? "blessing" : "sabotage", p.display, name);
            return;
        }

        log::Warn("{} failed for {}: {}", p.effectId, p.display, o.reason);
        // Money first, so anything reacting to these messages sees the refund already done.
        if (p.paid > 0) m_economy->Add(p.login, p.display, p.paid);
        m_web.Broadcast(std::format(R"({{"type":"failed","effect":"{}","reason":"{}"}})", JsonEscape(p.effectId), JsonEscape(o.reason)));
        if (p.paid > 0)
            Reply(p.simulated, std::format("@{} {} couldn't run ({}), so {} points were refunded.", p.display, name, o.reason, p.paid));
    }

    void Mod::Alert(const std::string& kind, const std::string& viewer, const std::string& text)
    {
        m_web.Broadcast(std::format(R"({{"type":"{}","viewer":"{}","text":"{}"}})",
                                    JsonEscape(kind), JsonEscape(viewer), JsonEscape(text)));
    }

    // Created with common bots the first time, then read again whenever it changes, so a
    // streamer can hide a new bot mid-stream without restarting the game.
    void Mod::ReloadHiddenIfChanged()
    {
        const auto path = HiddenPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) std::ofstream(path, std::ios::binary) << kHiddenTemplate;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) return;
        {
            std::lock_guard lock(m_hiddenMutex);
            if (stamp == m_hiddenTime) return;
        }

        std::set<std::string> hidden;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (StartsWith(line, "\xEF\xBB\xBF")) line.erase(0, 3); // Notepad's UTF-8 marker
            std::string login = Lower(Trim(line));
            if (StartsWith(login, "@")) login.erase(0, 1);
            if (IsValidLogin(login)) hidden.insert(login); // skips comments and blank lines too
        }
        log::Info("Leaving {} accounts in leaderboard_hidden.txt off the leaderboard", hidden.size());

        std::lock_guard lock(m_hiddenMutex);
        m_hidden = std::move(hidden);
        m_hiddenTime = stamp;
    }

    // Like leaderboard_hidden.txt: created with defaults, then read again whenever it changes.
    void Mod::ReloadRewardsIfChanged()
    {
        const auto path = RewardsPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) std::ofstream(path, std::ios::binary) << kRewardsTemplate;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) return;
        {
            std::lock_guard lock(m_rewardsMutex);
            if (stamp == m_rewardsTime) return;
        }

        std::map<std::string, long long> rewards;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (StartsWith(line, "\xEF\xBB\xBF")) line.erase(0, 3); // Notepad's UTF-8 marker
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            // The last = splits, so a title can contain one.
            const size_t eq = line.rfind('=');
            if (eq == std::string::npos) continue;
            const std::string title = Lower(Trim(line.substr(0, eq)));
            const int points = ToInt(Trim(line.substr(eq + 1)), 0);
            if (!title.empty() && points > 0) rewards[title] = std::min(points, 10000000);
        }
        log::Info("{} channel point rewards in channel_point_rewards.txt give points", rewards.size());

        std::lock_guard lock(m_rewardsMutex);
        m_rewards = std::move(rewards);
        m_rewardsTime = stamp;
    }

    // ── Web ─────────────────────────────────────────────────────────────────────

    HttpReply Mod::HandleHttp(const HttpRequest& req)
    {
        if (req.method == "GET" && req.path == "/state") return { 200, "application/json", StateJson() };
        if (req.method == "GET" && req.path == "/api/settings") return { 200, "application/json", SettingsJson() };
        if (req.method != "POST") return { 404, "text/plain", "not found" };

        auto form = ParseForm(req.body);
        auto field = [&](const char* key) { auto it = form.find(key); return it == form.end() ? std::string{} : it->second; };

        if (req.path == "/api/set")
        {
            const std::string value = field("value");
            if (value.empty() || !m_settings.Set(field("key"), ToInt(value, 0))) return JsonError(400, "unknown setting");
            return JsonOk();
        }
        if (req.path == "/api/fire")
        {
            const EffectDef* e = FindEffect(field("id"));
            if (!e) return JsonError(400, "unknown effect");
            if (!GameAlive()) return JsonError(400, "the game isn't running");
            Purchase p;
            p.effectId = e->id;
            p.display = "Control Panel";
            p.force = true;
            p.simulated = true;
            if (!Enqueue(std::move(p))) return JsonError(400, "the queue is full");
            return JsonOk();
        }
        if (req.path == "/api/control")
        {
            const std::string action = field("action");
            if (action == "pause") SetEnabled(false);
            else if (action == "resume") SetEnabled(true);
            else if (action == "clear") ClearQueue();
            else if (action == "chaos") { if (!FireChaosGods()) return JsonError(400, "nothing is available to fire"); }
            else return JsonError(400, "unknown action");
            return JsonOk();
        }
        if (req.path == "/api/simulate")
        {
            m_chat.InjectLine(StripLineBreaks(field("line")));
            return JsonOk();
        }
        if (req.path == "/api/simulate-eventsub")
        {
            m_eventSub.InjectMessage(field("message"));
            return JsonOk();
        }
        return { 404, "text/plain", "not found" };
    }

    std::string Mod::StateJson()
    {
        std::string shop;
        for (auto& e : AllEffects())
        {
            if (!m_settings.EffectEnabled(e.id)) continue;
            if (!shop.empty()) shop += ',';
            shop += std::format(R"({{"id":"{}","name":"{}","price":{},"tier":"{}"}})",
                                e.id, JsonEscape(e.name), m_settings.Price(e.id, e.defaultPrice), TierName(e.tier));
        }
        // The streamer and the channel's bots aren't viewers, so they stay off the board, though
        // they keep their points. The streamer can mint points with !give, and bots earn them by
        // chatting, so either could otherwise top it. Any name with "bot" in it counts as a bot,
        // which catches most of them, along with the odd viewer (an "abbott").
        std::set<std::string> hidden;
        {
            std::lock_guard lock(m_hiddenMutex);
            hidden = m_hidden;
        }
        hidden.insert(m_boot.channel);
        hidden.insert(m_boot.botUsername);
        const auto offBoard = [&](const std::string& login)
        {
            return hidden.contains(login) || login.find("bot") != std::string::npos;
        };
        std::string top;
        for (auto& v : m_economy->Top(5, offBoard))
        {
            if (!top.empty()) top += ',';
            top += std::format(R"({{"name":"{}","points":{}}})", JsonEscape(v.display), v.points);
        }
        std::string active;
        for (auto& a : m_engine.Snapshot())
        {
            if (!active.empty()) active += ',';
            active += std::format(R"({{"id":"{}","name":"{}","remaining":{:.1f},"duration":{:.1f}}})",
                                  JsonEscape(a.id), JsonEscape(a.name), a.remaining, a.duration);
        }
        size_t queued;
        {
            std::lock_guard lock(m_queueMutex);
            queued = m_queue.size();
        }
        const bool alive = GameAlive();
        const Block block = (Block)m_block.load();
        return std::format(R"({{"game":{},"ready":{},"gameStatus":"{}","twitch":{},"twitchConfigured":{},"channelPoints":{},"enabled":{},"queued":{},"active":[{}],"shop":[{}],"leaderboard":[{}]}})",
                           alive, alive && block == Block::None, JsonEscape(alive ? BlockReason(block) : "not running"),
                           m_chat.IsConnected(), m_boot.TwitchConfigured(), m_eventSub.IsListening(), m_settings.Get("Enabled") != 0,
                           queued, active, shop, top);
    }

    std::string Mod::SettingsJson()
    {
        std::string settings;
        for (auto& d : SettingDefs())
        {
            if (!settings.empty()) settings += ',';
            settings += std::format(R"({{"key":"{}","group":"{}","label":"{}","help":"{}","isBool":{},"min":{},"max":{},"default":{},"value":{}}})",
                                    d.key, JsonEscape(d.group), JsonEscape(d.label), JsonEscape(d.help), d.isBool, d.min, d.max, d.def, m_settings.Get(d.key));
        }
        std::string effects;
        for (auto& e : AllEffects())
        {
            if (!effects.empty()) effects += ',';
            effects += std::format(R"({{"id":"{}","name":"{}","tier":"{}","description":"{}","defaultPrice":{},"price":{},"enabled":{},"needsVehicle":{},"needsOnFoot":{},"risky":{},"silent":{}}})",
                                   e.id, JsonEscape(e.name), TierName(e.tier), JsonEscape(e.description), e.defaultPrice,
                                   m_settings.Price(e.id, e.defaultPrice), m_settings.EffectEnabled(e.id),
                                   (e.flags & NeedsVehicle) != 0, (e.flags & NeedsOnFoot) != 0, (e.flags & RiskyInMission) != 0, (e.flags & Silent) != 0);
        }
        return std::format(R"({{"version":"{}","dataDir":"{}","settings":[{}],"effects":[{}]}})",
                           kVersion, JsonEscape(DataDir().string()), settings, effects);
    }
}
