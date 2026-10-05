/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — file-driven prompt engine (SPEC-002 §2).
 *
 * No prompt text lives in C++. Every request type (player_chat, npc_chat, ambient, …) is a
 * folder under <AISystem.PromptRoot>/prompts/<type>/ holding slot .md templates assembled by
 * a slots.txt manifest; missing slots fall back to prompts/shared/. NPC bios live in
 * <PromptRoot>/characters/<entry> - <name>.md (per creature entry), auto-created if missing.
 *
 * Templates are cached with mtime invalidation: editing a file is picked up on the next
 * request (hot reload) or by calling Reload() (the `.ai reload` command).
 */

#ifndef TRINITY_AI_PROMPT_H
#define TRINITY_AI_PROMPT_H

#include "Define.h"
#include <string>
#include <utility>
#include <vector>
#include <map>
#include <filesystem>

namespace fs = std::filesystem;

struct AIPromptInput
{
    std::string type;        // "player_chat" | "npc_chat" | "ambient" (folder under prompts/)
    std::string npcName;     // speaking/responding NPC display name
    uint32      npcEntry = 0;// creature_template.entry
    std::string zone;        // map / zone name
    std::string playerName;  // calling player (may be empty)
    std::string targetName;  // addressee (may be empty)
    std::string lastLine;    // the line being answered (may be empty)
    std::string language;    // resolved response language (AISystem.Language / per-NPC)
    std::string location;    // "Zone (map id) @ x,y" (may be empty)
    std::string time;        // "HH:MM" current server time

    // --- Spec 003: NPC "vitals" (self-awareness) ---
    // Pre-resolved by AIManager::BuildVitals so AIPrompt stays free of game-type lookups.
    std::string race;        // creature family name, or creature type string if family is none
    std::string gender;      // "male" | "female" | "none" | "unknown"
    std::string faction;     // faction display name (or numeric id if lookup fails)
    std::string subname;     // creature_template.SubName (title); may be empty
    std::string equipment;   // "- none" or "- <item name>" per equipped slot
};

class TC_GAME_API AIPrompt
{
public:
    // Seeds the default template tree + indexes existing bios. Call once at startup.
    static bool Initialize();
    // Flushes all caches + re-indexes the characters/ folder. Call from `.ai reload`.
    static void Reload();

    static fs::path PromptRoot();

    // Returns the NPC's bio text; auto-creates "<entry> - <name>.md" (empty) if missing.
    static std::string GetBio(uint32 entry, std::string const& name);

    // Assembles the {system, userContext} pair for a request type from its slot files.
    static std::pair<std::string, std::string> Build(AIPromptInput const& in,
                                                     std::string const& bio,
                                                     std::string const& memories,
                                                     std::string const& summary,
                                                     std::string const& events);

    // Resolves {{placeholders}} in a template using the input + injected slot content.
    static std::string Resolve(std::string const& tmpl, AIPromptInput const& in,
                               std::string const& bio, std::string const& memories,
                               std::string const& summary, std::string const& events);

    // Current server time as "HH:MM" (used for {{time}}).
    static std::string CurrentTime();

private:
    // Loads a slot template (cache + mtime), falling back to shared/ then built-in.
    static std::string LoadSlot(std::string const& type, std::string const& slot);
    // Parses the type's slots.txt (file : role), defaulting to the standard six slots.
    static std::vector<std::pair<std::string, std::string>> LoadSlots(std::string const& type);
    // Creates the default template tree on first run.
    static void SeedDefaults();
    // Indexes characters/<entry> - name.md files into entry -> path.
    static void IndexBios();
    // Read a file's current content if its mtime changed since the cache.
    static std::string ReadCached(std::string const& path);
};

#define sAIPrompt AIPrompt

#endif // TRINITY_AI_PROMPT_H
