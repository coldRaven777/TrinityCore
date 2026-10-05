/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIManager.h"
#include "AI/LivingNPC/AIConfig.h"
#include "AI/LivingNPC/AIService.h"
#include "AI/LivingNPC/AIContext.h"
#include "AI/LivingNPC/AIPrompt.h"
#include "AI/LivingNPC/AIDecision.h"
#include "AI/LivingNPC/AIEvent.h"

#include "World.h"
#include "WorldSession.h"
#include "Player.h"
#include "Creature.h"
#include "Map.h"
#include "GameTime.h"
#include "Log.h"
#include "Util.h"

#include "ObjectMgr.h"          // sObjectMgr: items, faction templates, equipment
#include "DB2Stores.h"          // sFactionStore, sCreatureFamilyStore
#include "SharedDefines.h"      // CreatureType, CreatureFamily, Gender
#include "ItemTemplate.h"       // ItemTemplate::GetName

#include <cmath>
#include <random>
#include <algorithm>

AIManager* AIManager::Get()
{
    static AIManager instance;
    return &instance;
}

bool AIManager::IsEnabled() const
{
    return sAIConfig->IsEnabled();
}

void AIManager::Initialize()
{
    sAIConfig->Load();
    // Prompt templates + event log initialize INDEPENDENTLY of AISystem.Enabled:
    // Spec 002 seeds the prompt tree on first boot and keeps the dashboard fed with
    // events even when AI replies are off (gated only by player proximity).
    AIPrompt::Initialize();
    AIEvent::Initialize();

    if (sAIConfig->IsEnabled())
    {
        AIService::Get()->Start();
        TC_LOG_INFO("AISystem", "AIManager initialized (feature enabled).");
    }
    else
    {
        TC_LOG_INFO("AISystem", "AIManager initialized (feature DISABLED in config).");
    }
    m_initialized = true;
}

void AIManager::SetSessionEnabled(uint32 accountId, bool on)
{
    std::lock_guard<std::mutex> g(m_sessionLock);
    if (accountId == 0)
        m_globalEnabled = on;
    else
        m_sessionEnabled[accountId] = on;
}

bool AIManager::IsSessionEnabled(uint32 accountId) const
{
    std::lock_guard<std::mutex> g(m_sessionLock);
    if (accountId == 0)
        return m_globalEnabled;
    auto it = m_sessionEnabled.find(accountId);
    if (it != m_sessionEnabled.end())
        return it->second;
    return m_globalEnabled;
}

static bool SessionAllowed(Player* player)
{
    if (!player || !player->GetSession())
        return sAIManager->IsSessionEnabled(0);
    return sAIManager->IsSessionEnabled(player->GetSession()->GetAccountId());
}

// Collect creatures on the same map within `radius` yards of a world object.
static std::vector<Creature*> CollectNearby(WorldObject const* center, float radius)
{
    std::vector<Creature*> out;
    if (!center)
        return out;
    Map* map = center->GetMap();
    if (!map)
        return out;
    for (auto const& kv : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = kv.second;
        if (!c || !c->IsAlive() || c == center)
            continue;
        if (center->GetDistance(c) <= radius)
            out.push_back(c);
    }
    return out;
}

// Spec 002 fix: only humanoid and mechanical creatures may be AI speakers.
// Gate is configurable via AISystem.SpeakableOnlyHumanoidMechanical (default ON).
static bool CanSpeak(Creature* c)
{
    if (!c)
        return false;
    if (!sAIConfig->SpeakableOnlyHumanoidMechanical())
        return true;
    uint32 type = c->GetCreatureType();
    return type == CREATURE_TYPE_HUMANOID || type == CREATURE_TYPE_MECHANICAL;
}

static void EraseNonSpeakers(std::vector<Creature*>& list)
{
    list.erase(std::remove_if(list.begin(), list.end(),
        [](Creature* c) { return !CanSpeak(c); }), list.end());
}

static std::string LocationString(Creature* c)
{
    if (!c || !c->GetMap())
        return "";
    return Trinity::StringFormat("{} (map {}) @ {}, {}", c->GetMap()->GetMapName(), c->GetMap()->GetId(),
        std::to_string(c->GetPositionX()), std::to_string(c->GetPositionY()));
}

static std::string PlayerUid(Player* p)
{
    return "player:" + std::to_string(p->GetGUID().GetCounter());
}

static std::string NpcUid(Creature* c)
{
    return "npc_spawn:" + std::to_string(uint64(c->GetSpawnId()));
}

// Spec 003: map a CreatureType enum to a human-readable string (no DB2 dependency).
static char const* CreatureTypeString(uint32 type)
{
    switch (type)
    {
        case CREATURE_TYPE_BEAST:        return "Beast";
        case CREATURE_TYPE_DRAGONKIN:    return "Dragonkin";
        case CREATURE_TYPE_DEMON:        return "Demon";
        case CREATURE_TYPE_ELEMENTAL:    return "Elemental";
        case CREATURE_TYPE_GIANT:        return "Giant";
        case CREATURE_TYPE_UNDEAD:       return "Undead";
        case CREATURE_TYPE_HUMANOID:     return "Humanoid";
        case CREATURE_TYPE_CRITTER:      return "Critter";
        case CREATURE_TYPE_MECHANICAL:   return "Mechanical";
        case CREATURE_TYPE_NOT_SPECIFIED:return "Unknown";
        case CREATURE_TYPE_TOTEM:        return "Totem";
        case CREATURE_TYPE_NON_COMBAT_PET: return "Non-combat Pet";
        case CREATURE_TYPE_GAS_CLOUD:    return "Gas Cloud";
        case CREATURE_TYPE_WILD_PET:     return "Wild Pet";
        case CREATURE_TYPE_ABERRATION:   return "Aberration";
        default:                         return "Unknown";
    }
}

// Spec 003: build the NPC "vitals" strings (race/family, gender, faction, subname, equipment)
// so the prompt assembler (AIPrompt) stays free of game-type lookups.
static void BuildVitals(Creature* c, AIPromptInput& in)
{
    if (!c)
        return;
    CreatureTemplate const* cInfo = c->GetCreatureTemplate();
    if (!cInfo)
        return;

    LocaleConstant loc = sWorld ? sWorld->GetDefaultDbcLocale() : DEFAULT_LOCALE;

    // race/family: prefer the creature family name; fall back to creature type string.
    {
        std::string race = "Unknown";
        if (cInfo->family != CREATURE_FAMILY_NONE)
            if (CreatureFamilyEntry const* fam = sCreatureFamilyStore.LookupEntry(cInfo->family))
                if (char const* n = fam->Name.Str[loc])
                    race = n;
        if (race == "Unknown" && cInfo->type != 0)
            race = CreatureTypeString(cInfo->type);
        in.race = race;
    }

    // gender
    switch (c->GetGender())
    {
        case GENDER_MALE:   in.gender = "male";   break;
        case GENDER_FEMALE: in.gender = "female"; break;
        case GENDER_NONE:   in.gender = "none";   break;
        default:            in.gender = "unknown"; break;
    }

    // faction: creature_template.faction -> FactionTemplate -> Faction name
    {
        std::string faction = std::to_string(cInfo->faction);
        if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(cInfo->faction))
            if (FactionEntry const* f = sFactionStore.LookupEntry(ft->Faction))
                if (char const* n = f->Name.Str[loc])
                    if (n[0] != '\0')
                        faction = n;
        in.faction = faction;
    }

    // subname (title)
    in.subname = cInfo->SubName;

    // equipment
    {
        std::string eq;
        EquipmentInfo const* einfo = nullptr;
        int8 id = static_cast<int8>(c->GetCurrentEquipmentId());
        einfo = sObjectMgr->GetEquipmentInfo(c->GetEntry(), id);
        if (!einfo && id == 0)
            einfo = sObjectMgr->GetEquipmentInfo(c->GetEntry(), id = 1); // try the common "default" template
        bool any = false;
        if (einfo)
        {
            for (uint8 i = 0; i < MAX_EQUIPMENT_ITEMS; ++i)
            {
                uint32 itemId = einfo->Items[i].ItemId;
                if (itemId == 0)
                    continue;
                ItemTemplate const* it = sObjectMgr->GetItemTemplate(itemId);
                if (!it)
                    continue;
                eq += "- " + std::string(it->GetName(loc)) + "\n";
                any = true;
            }
        }
        if (!any)
            eq = "- none\n";
        in.equipment = eq;
    }
}

// Spec 003: sort candidate creatures by distance to `center` (ascending). Returns a new vector.
static std::vector<Creature*> SortByDistance(WorldObject const* center, std::vector<Creature*> const& list)
{
    std::vector<Creature*> out = list;
    if (center)
        std::sort(out.begin(), out.end(),
            [center](Creature* a, Creature* b)
            {
                if (!a) return false;
                if (!b) return true;
                return center->GetDistance(a) < center->GetDistance(b);
            });
    return out;
}

void AIManager::OnPlayerSpeak(Player* player, std::string const& text)
{
    if (!player || text.empty())
        return;

    // Spec 002 §3: log the event FIRST (independent of AISystem.Enabled). The logger
    // internally gates on AISystem.EventLog + player proximity.
    AIEvent::LogPlayerSay(player, text);

    if (!IsEnabled() || !SessionAllowed(player))
        return;

    float radius = sAIConfig->EarshotRadius();
    std::vector<Creature*> nearby = CollectNearby(player, radius);
    EraseNonSpeakers(nearby);   // Spec 002 fix: animals/etc. do not talk
    if (nearby.empty())
        return;

    // Spec 003: rank by distance to the player so the rule fallback picks the closest NPC,
    // not a far one collected later by the map store.
    nearby = SortByDistance(player, nearby);

    Creature* responder = nullptr;

    // 1) Selected creature (and within earshot)
    if (Unit* sel = player->GetSelectedUnit())
        if (Creature* sc = sel->ToCreature())
            if (player->GetDistance(sc) <= radius)
                responder = sc;

    // 2) A creature directly in front of the player
    if (!responder)
    {
        float orient = player->GetOrientation();
        float fx = std::cos(orient), fy = std::sin(orient);
        Creature* best = nullptr;
        float bestDot = 0.3f; // require somewhat in front
        for (Creature* c : nearby)
        {
            float dx = c->GetPositionX() - player->GetPositionX();
            float dy = c->GetPositionY() - player->GetPositionY();
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < 0.001f) continue;
            float dot = (dx / d) * fx + (dy / d) * fy;
            if (dot > bestDot)
            {
                bestDot = dot;
                best = c;
            }
        }
        responder = best;
    }

    // 3) Decision model / rule fallback among nearby
    if (!responder)
    {
        if (sAIConfig->UseDecisionModel() && nearby.size() > 1 && sAIConfig->HasValidKey())
        {
            std::vector<std::pair<std::string, std::string>> cands;
            for (Creature* c : nearby)
                cands.push_back({ std::to_string(uint64(c->GetSpawnId())),
                    c->GetName() + " (dist=" + std::to_string(int(player->GetDistance(c))) + "yd)" });
            std::string guid = AIDecision::Decide(sAIConfig->OpenRouterApiKey(), sAIConfig->DecisionModel(), cands, text, sAIConfig->RequestTimeoutSec());
            if (!guid.empty())
                for (Creature* c : nearby)
                    if (std::to_string(uint64(c->GetSpawnId())) == guid) { responder = c; break; }
        }
        if (!responder)
            responder = nearby.front(); // Spec 003: closest nearby (distance-sorted above)
    }

    if (responder)
        RequestReply(responder, player, text, false, "player_chat");

    // Nearby NPCs may interject.
    MaybeInterject(responder, player, text);
}

void AIManager::OnNpcSpeak(Creature* speaker, std::string const& text)
{
    if (!speaker || text.empty())
        return;

    // Spec 002 §3: log the NPC speech (gated internally by EventLog + player proximity).
    AIEvent::LogNpcSay(speaker, text);

    if (!IsEnabled())
        return;

    // Spec 002 §3.6: the AI engine only works in a player's immediate periphery.
    if (!AIEvent::PlayerNear(speaker, sAIConfig->ActivationRadius()))
        return;

    float radius = sAIConfig->EarshotRadius();
    std::vector<Creature*> others = CollectNearby(speaker, radius);
    EraseNonSpeakers(others);   // Spec 002 fix: animals/etc. do not talk
    if (others.empty())
        return;

    // Spec 003: rank candidates by distance to the *speaker* first, so the responder is
    // the NPC actually next to the speaker (not an out-of-frame far one).
    others = SortByDistance(speaker, others);

    Creature* responder = nullptr;
    if (others.size() == 1)
    {
        responder = others.front();
    }
    else if (sAIConfig->UseDecisionModel() && sAIConfig->HasValidKey())
    {
        // Build distance-annotated candidates for the decision model.
        std::vector<std::pair<std::string, std::string>> cands;
        for (Creature* c : others)
            cands.push_back({ std::to_string(uint64(c->GetSpawnId())),
                c->GetName() + " (dist=" + std::to_string(int(speaker->GetDistance(c))) + "yd)" });
        std::string guid = AIDecision::Decide(sAIConfig->OpenRouterApiKey(), sAIConfig->DecisionModel(), cands, text, sAIConfig->RequestTimeoutSec());
        if (!guid.empty())
            for (Creature* c : others)
                if (std::to_string(uint64(c->GetSpawnId())) == guid) { responder = c; break; }
    }
    if (!responder)
        responder = others.front(); // Spec 003: closest other (distance-sorted above)

    if (responder)
    {
        std::string userLine = "[" + std::string(speaker->GetName()) + "] said: " + text;
        RequestReply(responder, nullptr, userLine, false, "npc_chat");
    }
}

void AIManager::OnUnitDeath(Unit* victim, Unit* killer)
{
    AIEvent::LogDeath(victim, killer);
}

void AIManager::OnUnitCombatStart(Unit* owner, Unit* enemy)
{
    AIEvent::LogCombatStart(owner, enemy);
}

void AIManager::OnUnitCombatEnd(Unit* owner)
{
    AIEvent::LogCombatEnd(owner);
}

void AIManager::RequestReply(Creature* speaker, Player* audience, std::string const& userLine, bool isInterject, std::string const& type)
{
    if (!speaker)
        return;

    // Spec 002 fix: only humanoid/mechanical creatures may be AI speakers.
    if (!CanSpeak(speaker))
        return;

    // Spec 002 §3.6: the AI engine only works in a player's immediate periphery.
    if (!AIEvent::PlayerNear(speaker, sAIConfig->ActivationRadius()))
        return;

    // Per-NPC cooldown gate.
    uint64 key = uint64(speaker->GetSpawnId());
    uint64 now = GameTime::GetGameTime();
    {
        std::lock_guard<std::mutex> g(m_cooldownLock);
        auto it = m_npcCooldown.find(key);
        if (it != m_npcCooldown.end() && now < it->second)
            return; // still cooling down
        m_npcCooldown[key] = now + sAIConfig->CooldownSec();
    }

    // ---- Build the file-driven prompt (SPEC-002 §2) ----
    AIPromptInput in;
    in.type = type;
    in.npcName = speaker->GetName();
    in.npcEntry = speaker->GetEntry();
    if (Map* m = speaker->GetMap())
        in.zone = m->GetMapName();
    in.playerName = audience ? audience->GetName() : "";
    in.targetName = audience ? audience->GetName() : "";
    in.lastLine = userLine;
    in.language = sAIConfig->Language();
    in.location = LocationString(speaker);
    in.time = AIPrompt::CurrentTime();
    BuildVitals(speaker, in); // Spec 003: self-awareness block

    std::string convKey = std::to_string(key);

    std::string bio = AIPrompt::GetBio(speaker->GetEntry(), speaker->GetName());
    std::string memories; // Spec 003 fills {{memories}}; empty for now.
    std::string summary = m_context.GetSummary(convKey, sAIConfig->SummaryChars());

    std::vector<std::string> involved;
    involved.push_back(NpcUid(speaker));
    if (audience)
        involved.push_back(PlayerUid(audience));
    std::string events = AIEvent::RecentEventsFor(speaker->GetMapId(), involved,
        sAIConfig->MaxInjectedEvents(), sAIConfig->EventTrimChars());

    auto [system, userCtx] = AIPrompt::Build(in, bio, memories, summary, events);

    std::vector<AIMessage> messages = m_context.BuildMessages(convKey, system, userCtx,
        sAIConfig->ContextHistoryRatio(), sAIConfig->MaxTokens());

    // Record our side of the exchange for history. (Response added after drain.)
    uint32 mapId = speaker->GetMapId();
    uint32 instanceId = speaker->GetInstanceId();
    uint32 spawnId = speaker->GetSpawnId();
    std::string replyRole = "assistant";

    AIService::Get()->RequestChatCompletion(sAIConfig->OpenRouterApiKey(), sAIConfig->ChatModel(),
        messages, sAIConfig->Temperature(), sAIConfig->MaxTokens(), sAIConfig->RequestTimeoutSec(),
        [this, convKey, userLine, replyRole, mapId, instanceId, spawnId, isInterject](bool success, std::string reply, std::string error)
        {
            if (!success)
            {
                TC_LOG_ERROR("AISystem", "Chat request failed: {}", error);
                return;
            }
            // Trim reply to a sane length and strip surrounding whitespace/quotes.
            std::string line = reply;
            size_t a = line.find_first_not_of(" \t\r\n\"");
            if (a != std::string::npos)
            {
                size_t b = line.find_last_not_of(" \t\r\n\"");
                if (b != std::string::npos) line = line.substr(a, b - a + 1);
            }
            if (line.size() > 2000) line = line.substr(0, 2000);

            // Update history on the game-thread-safe context (mutex guarded).
            sAIManager->m_context.AddTurn(convKey, "user", userLine);
            sAIManager->m_context.AddTurn(convKey, replyRole, line);

            AIResult r;
            r.mapId = mapId;
            r.instanceId = instanceId;
            r.spawnId = spawnId;
            r.text = std::move(line);
            r.isInterject = isInterject;
            EnqueueResult(std::move(r));
        });
}

void AIManager::MaybeInterject(Creature* speaker, Player* audience, std::string const& saidLine)
{
    if (!speaker)
        return;
    float chance = sAIConfig->InterjectChance();
    if (chance <= 0.0f)
        return;
    // Simple RNG gate.
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    if (dist(rng) > chance)
        return;

    float radius = sAIConfig->EarshotRadius();
    std::vector<Creature*> nearby = CollectNearby(speaker, radius);
    // Spec 003: rank by distance to the speaker so the interjector is the closest other NPC,
    // not an arbitrary map-store entry.
    nearby = SortByDistance(speaker, nearby);
    Creature* interjector = nullptr;
    for (Creature* c : nearby)
        if (c != speaker && CanSpeak(c)) { interjector = c; break; }   // Spec 002 + Spec 003 fix
    if (!interjector)
        return;

    std::string userLine = "You are nearby and overhear this. React briefly in one short sentence: ["
        + std::string(speaker->GetName()) + "] said: " + saidLine;
    RequestReply(interjector, audience, userLine, true, "player_chat");
}

void AIManager::EnqueueResult(AIResult&& r)
{
    std::lock_guard<std::mutex> g(m_queueLock);
    m_results.push(std::move(r));
}

void AIManager::Update(uint32 diff)
{
    if (!IsEnabled())
        return;

    // Drain completed LLM results and make creatures speak (game thread).
    std::queue<AIResult> ready;
    {
        std::lock_guard<std::mutex> g(m_queueLock);
        std::swap(ready, m_results);
    }
    while (!ready.empty())
    {
        AIResult r = std::move(ready.front());
        ready.pop();
        Map* map = sMapMgr->FindMap(r.mapId, r.instanceId);
        Creature* c = map ? map->GetCreatureBySpawnId(r.spawnId) : nullptr;
        if (c && c->IsAlive())
        {
            c->Say(r.text, LANG_UNIVERSAL);
            // Log the LLM-produced speech line as an npc_say event (Spec 002 §3).
            AIEvent::LogNpcSay(c, r.text);
            TC_LOG_DEBUG("AISystem", "NPC {} says: {}", c->GetName(), r.text);
        }
    }

    UpdateAmbient(diff);
}

void AIManager::UpdateAmbient(uint32 diff)
{
    uint32 cooldown = sAIConfig->NpcChatCooldownSec();
    if (cooldown == 0)
        return; // ambient disabled

    // Only run when a player is present, if configured.
    if (sAIConfig->AmbientNeedsPlayer() && sWorld->GetActiveSessionCount() == 0)
        return;

    // Spec 003: global cooldown across ALL maps so narrator calls don't stack/back-to-back.
    uint64 now = GameTime::GetGameTime();
    uint32 globalCd = sAIConfig->AmbientGlobalCooldownSec();
    if (globalCd > 0 && m_lastAmbientFire != 0 && (now - m_lastAmbientFire) < globalCd)
        return;

    // Spec 003: burst cap (per rolling minute). 0 = unlimited.
    uint32 maxPerMin = sAIConfig->AmbientMaxPerMinute();
    if (maxPerMin > 0)
    {
        if (m_ambientWindowStart == 0)
            m_ambientWindowStart = now;
        if (now - m_ambientWindowStart >= 60)
        {
            m_ambientWindowStart = now;
            m_ambientWindowCount = 0;
        }
        if (m_ambientWindowCount >= maxPerMin)
            return;
    }

    m_ambientTimer += diff;
    if (m_ambientTimer < cooldown * 1000)
        return;
    m_ambientTimer = 0;

    // Use the first session player on their map.
    Player* ref = nullptr;
    for (auto const& s : sWorld->GetAllSessions())
    {
        if (s.second && s.second->GetPlayer() && s.second->GetPlayer()->IsInWorld())
        {
            ref = s.second->GetPlayer();
            break;
        }
    }
    if (!ref)
        return;

    // Spec 003: honor per-session "ai off" — don't narrate for a player who disabled AI.
    if (!SessionAllowed(ref))
        return;

    Map* map = ref->GetMap();
    if (!map)
        return;

    // Spec 002 §3.6: ambient chatter only within the player's immediate periphery,
    // and only humanoid/mechanical creatures may speak (Spec 002 fix).
    std::vector<Creature*> creatures;
    for (auto const& kv : map->GetCreatureBySpawnIdStore())
        if (kv.second && kv.second->IsAlive() && CanSpeak(kv.second))
            if (ref->GetDistance(kv.second) <= sAIConfig->ActivationRadius())
                creatures.push_back(kv.second);
    if (creatures.empty())
        return;

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, creatures.size() - 1);
    Creature* speaker = creatures[pick(rng)];

    // Spec 003: skip this tick if the chosen narrator has nothing to say (no events,
    // empty summary, empty bio) — avoids noisy "thinking aloud" filler that burns tokens.
    {
        std::string bio = AIPrompt::GetBio(speaker->GetEntry(), speaker->GetName());
        std::string convKey = std::to_string(uint64(speaker->GetSpawnId()));
        std::string summary = m_context.GetSummary(convKey, sAIConfig->SummaryChars());
        std::vector<std::string> involved = { NpcUid(speaker) };
        std::string events = AIEvent::RecentEventsFor(map->GetId(), involved,
            sAIConfig->MaxInjectedEvents(), sAIConfig->EventTrimChars());
        if (bio.empty() && summary.empty() && events.empty())
        {
            TC_LOG_DEBUG("AISystem", "Ambient: skipping narrator tick (no context for {}).", speaker->GetName());
            return; // don't call the model; reset already happened above
        }
    }

    // ---- Build the file-driven ambient prompt ----
    AIPromptInput in;
    in.type = "ambient";
    in.npcName = speaker->GetName();
    in.npcEntry = speaker->GetEntry();
    in.zone = map->GetMapName();
    in.playerName = ref->GetName();
    in.language = sAIConfig->Language();
    in.location = LocationString(speaker);
    in.time = AIPrompt::CurrentTime();
    BuildVitals(speaker, in); // Spec 003: self-awareness block

    std::string convKey = std::to_string(uint64(speaker->GetSpawnId()));

    std::string bio = AIPrompt::GetBio(speaker->GetEntry(), speaker->GetName());
    std::string memories; // Spec 003
    std::string summary;  // ambient does not need a recap
    std::vector<std::string> involved = { NpcUid(speaker) };
    std::string events = AIEvent::RecentEventsFor(map->GetId(), involved,
        sAIConfig->MaxInjectedEvents(), sAIConfig->EventTrimChars());

    auto [system, userCtx] = AIPrompt::Build(in, bio, memories, summary, events);

    std::vector<AIMessage> messages = m_context.BuildMessages(convKey, system, userCtx, 0.0f, sAIConfig->MaxTokens());

    uint32 mapId = speaker->GetMapId();
    uint32 instanceId = speaker->GetInstanceId();
    uint32 spawnId = speaker->GetSpawnId();

    // Spec 003: bookkeeping for global cooldown + burst cap.
    m_lastAmbientFire = GameTime::GetGameTime();
    ++m_ambientWindowCount;

    AIService::Get()->RequestChatCompletion(sAIConfig->OpenRouterApiKey(), sAIConfig->ChatModel(),
        messages, sAIConfig->Temperature(), sAIConfig->MaxTokens(), sAIConfig->RequestTimeoutSec(),
        [this, convKey, mapId, instanceId, spawnId](bool success, std::string reply, std::string error)
        {
            if (!success) { TC_LOG_ERROR("AISystem", "Ambient request failed: {}", error); return; }
            std::string line = reply;
            size_t a = line.find_first_not_of(" \t\r\n\"");
            if (a != std::string::npos) { size_t b = line.find_last_not_of(" \t\r\n\""); if (b != std::string::npos) line = line.substr(a, b - a + 1); }
            if (line.size() > 2000) line = line.substr(0, 2000);
            sAIManager->m_context.AddTurn(convKey, "assistant", line);
            AIResult r;
            r.mapId = mapId; r.instanceId = instanceId; r.spawnId = spawnId; r.text = std::move(line); r.isInterject = false;
            EnqueueResult(std::move(r));
        });
}
