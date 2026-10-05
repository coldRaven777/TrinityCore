/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — "who should reply" decision.
 * Uses a small/fast OpenRouter call (synchronous) to pick a responder among candidates,
 * with a rule-based fallback (closest other NPC) when the call fails or is disabled.
 */

#ifndef TRINITY_AI_DECISION_H
#define TRINITY_AI_DECISION_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class TC_GAME_API AIDecision
{
public:
    // candidates: pairs of (guid string, name). lastLine: the utterance that needs a reply.
    // Returns the chosen GUID as string, or "" if no candidate could be chosen.
    static std::string Decide(std::string const& apiKey,
                              std::string const& model,
                              std::vector<std::pair<std::string, std::string>> const& candidates,
                              std::string const& lastLine,
                              uint32 timeoutSec);

    // Rule-based fallback: pick the candidate whose name matches best or just the first.
    static std::string RuleFallback(std::vector<std::pair<std::string, std::string>> const& candidates);
};

#endif // TRINITY_AI_DECISION_H
