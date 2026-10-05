/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — AI event log (SPEC-002 §3).
 * - Writes immutable "facts" (say/emote/death/combat/presence) to the `ai_npc` schema
 *   through the dedicated AIDatabase pool, asynchronously (never blocks the game thread).
 * - Events are keyed by spawn GUID for NPCs (npc_spawn:<guid>) — per-INDIVIDUAL streams —
 *   with the creature entry carried separately so the dashboard/bios can join on it.
 * - Every event is gated by player proximity: nothing is logged unless a player is within
 *   AISystem.ActivationRadius of the actor (SPEC-002 §3.6).
 * - Event logging is INDEPENDENT of AISystem.Enabled, so the dashboard has data even when
 *   AI replies are off; rows not fed into an LLM prompt stay ai_processed = 0 (red badge).
 */

#ifndef TRINITY_AI_EVENT_H
#define TRINITY_AI_EVENT_H

#include "Define.h"
#include <string>
#include <vector>

class WorldObject;
class Creature;
class Unit;
class Player;

namespace AIEvent
{
    enum class Type : uint8
    {
        NpcSay = 0,
        PlayerSay,
        NpcEmote,
        Death,
        CombatInitiated,
        CombatEnded,
        NpcEvade,
        PlayerEnteredRange,
        Max
    };

    TC_GAME_API char const* TypeName(Type type);

    // Lifecycle ---------------------------------------------------------------
    // Opens the AIDatabase pool if AIDatabaseInfo is configured. Safe to call once
    // at world startup (from AIManager::Initialize). Never fails the server: if the
    // DB is absent/unreachable the feature just logs errors and idles.
    TC_GAME_API void Initialize();
    TC_GAME_API void Shutdown();
    TC_GAME_API bool IsReady();          // pool open && AISystem.EventLog enabled
    TC_GAME_API void Reload();           // re-check EventLog flag (after .ai reload)

    // Proximity gate (SPEC-002 §3.6): is any online player within `radius` of `obj`?
    TC_GAME_API bool PlayerNear(WorldObject const* obj, float radius);

    // Generic log — fire-and-forget async INSERT (caller already ran the gates).
    // NOTE: named LogEvent (not Log) so it does not shadow the global `Log` class used
    // by the TC_LOG_* macros inside namespace AIEvent.
    TC_GAME_API void LogEvent(Type type,
                         std::string const& actorUid, std::string const& actorName, uint32 actorEntry,
                         std::string const& targetUid, std::string const& targetName,
                         std::string const& content,
                         uint32 mapId, float x, float y, float z,
                         std::string const& location,
                         std::vector<std::string> const& related,
                         std::string const& payloadJson = "{}");

    // Convenience wrappers (each internally gates on IsReady + player proximity).
    TC_GAME_API void LogNpcSay(Creature* c, std::string const& text, std::string const& targetUid = "", std::string const& targetName = "");
    TC_GAME_API void LogPlayerSay(Player* p, std::string const& text);
    TC_GAME_API void LogNpcEmote(Creature* c, std::string const& emote);
    TC_GAME_API void LogDeath(Unit* victim, Unit* killer);
    TC_GAME_API void LogCombatStart(Unit* owner, Unit* enemy);
    TC_GAME_API void LogCombatEnd(Unit* owner);
    TC_GAME_API void LogEvade(Unit* owner);

    // Builds the {{events}} injection text for a prompt: recent ai_event rows whose
    // actor/target/related touches any of `involvedUids`, newest-first, formatted as
    // "[HH:MM] Name: content". Marks injected rows ai_processed = 1.
    TC_GAME_API std::string RecentEventsFor(uint32 mapId, std::vector<std::string> const& involvedUids,
                                            uint32 maxRows, uint32 charCap);
}

#endif // TRINITY_AI_EVENT_H
