/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — per-conversation rolling history with a context budget split.
 * No long-term memory yet: only the in-context history (system slice + history slice).
 */

#ifndef TRINITY_AI_CONTEXT_H
#define TRINITY_AI_CONTEXT_H

#include "Define.h"
#include "AIService.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>

class TC_GAME_API AIContext
{
public:
    AIContext() = default;

    // Adds an exchange to the rolling history for a given conversation key.
    // key is typically the speaker Creature GUID as string.
    void AddTurn(std::string const& key, std::string const& role, std::string const& content);

    // Builds the message list for a request: [system] + trimmed history + current user line.
    // systemPrompt is the persona. historyRatio/maxTokens come from config.
    std::vector<AIMessage> BuildMessages(std::string const& key,
                                         std::string const& systemPrompt,
                                         std::string const& currentUserLine,
                                         float historyRatio, uint32 maxTokens);

    // Estimate tokens from characters (rough): 1 token ~= 4 chars.
    static uint32 EstimateTokens(std::string const& s) { return uint32((s.size() / 4) + 1); }

    // Builds a naive recap of the conversation for the {{summary}} slot (SPEC-002 §2.3).
    // Concatenates recent turns ("Speaker: ..." / "You: ..."), keeping the NEWEST turns
    // that fit within maxChars. Persistent summaries arrive in Spec 003.
    std::string GetSummary(std::string const& key, uint32 maxChars);

private:
    std::mutex m_lock;
    std::map<std::string, std::vector<AIMessage>> m_history; // key -> rolling history
};

#endif // TRINITY_AI_CONTEXT_H
