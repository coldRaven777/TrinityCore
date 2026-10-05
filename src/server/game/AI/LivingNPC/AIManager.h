/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — orchestration manager.
 * - Loads config and starts the async HTTP service.
 * - Receives intents from hooks (player SAY, NPC say) and enqueues LLM requests.
 * - Drains completed results on the game thread (World::Update) and makes Creatures speak.
 * - Runs the ambient "narrator" timer (only when a player is present, if configured).
 */

#ifndef TRINITY_AI_MANAGER_H
#define TRINITY_AI_MANAGER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "AI/LivingNPC/AIContext.h"
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <unordered_map>

class Player;
class Creature;
class Unit;

struct AIResult
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 spawnId = 0;     // Creature spawn id (ObjectGuid low for creatures)
    std::string text;       // what to say
    bool isInterject = false;// whether this is an earshot interjection
};

class TC_GAME_API AIManager
{
public:
    static AIManager* Get();

    void Initialize();   // call once at world startup
    void Update(uint32 diff); // call from World::Update (game thread)

    // Hooks (called from chat / npc commands). These run on the game thread.
    void OnPlayerSpeak(Player* player, std::string const& text);
    void OnNpcSpeak(Creature* speaker, std::string const& text);

    // Game-flow hooks (SPEC-002 §3.6): death / combat. Log events; internally gated.
    void OnUnitDeath(Unit* victim, Unit* killer);
    void OnUnitCombatStart(Unit* owner, Unit* enemy);
    void OnUnitCombatEnd(Unit* owner);

    // Per-session toggle (ai on|off). Global fallback when no player given.
    void SetSessionEnabled(uint32 accountId, bool on);
    bool IsSessionEnabled(uint32 accountId) const;

    bool IsEnabled() const;

private:
    AIManager() = default;
    AIManager(AIManager const&) = delete;
    AIManager& operator=(AIManager const&) = delete;

    void RequestReply(Creature* speaker, Player* audience, std::string const& userLine, bool isInterject, std::string const& type);
    void MaybeInterject(Creature* speaker, Player* audience, std::string const& saidLine);
    void EnqueueResult(AIResult&& r);

    // Ambient narrator
    void UpdateAmbient(uint32 diff);

    // Result queue (populated by AIService callbacks, drained on game thread)
    mutable std::mutex m_queueLock;
    std::queue<AIResult> m_results;

    // Per-NPC last-reply timestamps (game-time seconds)
    std::unordered_map<uint64, uint64> m_npcCooldown;
    mutable std::mutex m_cooldownLock;

    // Session enable map (accountId -> enabled)
    std::unordered_map<uint32, bool> m_sessionEnabled;
    mutable std::mutex m_sessionLock;
    bool m_globalEnabled = true;

    // Ambient timer
    uint32 m_ambientTimer = 0;
    uint64 m_lastAmbientFire = 0;      // Spec 003: global cooldown across all maps
    uint64 m_ambientWindowStart = 0;   // Spec 003: rolling-minute window start
    uint32 m_ambientWindowCount = 0;   // Spec 003: ambient fires in the current window
    bool m_initialized = false;

    // Rolling conversation history (mutex-guarded internally).
    AIContext m_context;
};

#define sAIManager AIManager::Get()

#endif // TRINITY_AI_MANAGER_H
