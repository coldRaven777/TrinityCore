/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIDecision.h"
#include "AI/LivingNPC/AIService.h"
#include "Log.h"

#include <algorithm>

std::string AIDecision::Decide(std::string const& apiKey, std::string const& model,
                               std::vector<std::pair<std::string, std::string>> const& candidates,
                               std::string const& lastLine, uint32 timeoutSec)
{
    if (candidates.empty())
        return "";
    if (candidates.size() == 1)
        return candidates.front().first;

    // Build a compact decision prompt.
    std::string list;
    for (size_t i = 0; i < candidates.size(); ++i)
        list += std::to_string(i + 1) + ". GUID=" + candidates[i].first + " Name=" + candidates[i].second + "\n";

    std::string system = "You are a narrator directing an RPG scene. Given the nearby NPCs and the "
        "last line, decide which single NPC should reply next. Candidates are listed with their "
        "distance in yards from the speaking NPC (the one who just spoke). Prefer the CLOSEEST "
        "candidate that makes sense in context; do not pick a distant NPC just because its name "
        "appears first. Reply with ONLY that NPC's GUID value (the number after 'GUID='), nothing "
        "else. No explanation.";
    std::string user = "Nearby NPCs:\n" + list + "\nLast line: \"" + lastLine + "\"\nWhich NPC replies? Reply with only its GUID number.";

    std::vector<AIMessage> messages = { { "system", system }, { "user", user } };

    std::string reply;
    if (AIService::Get()->RequestChatCompletionSync(apiKey, model, messages, 0.2f, 16, timeoutSec, reply))
    {
        // Extract a GUID-like token from the reply (digits).
        // Our GUID strings are decimal numbers; find the first run of digits.
        size_t pos = reply.find_first_of("0123456789");
        if (pos != std::string::npos)
        {
            size_t end = reply.find_first_not_of("0123456789", pos);
            std::string guid = reply.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            // Validate it matches a candidate.
            for (auto const& c : candidates)
                if (c.first == guid)
                    return guid;
            // Otherwise fall through to rule-based.
        }
        TC_LOG_DEBUG("AISystem", "Decision model returned no valid GUID, falling back to rule.");
    }
    return RuleFallback(candidates);
}

std::string AIDecision::RuleFallback(std::vector<std::pair<std::string, std::string>> const& candidates)
{
    if (candidates.empty())
        return "";
    // Trivial but stable: pick the candidate whose name is lexicographically "first".
    auto it = std::min_element(candidates.begin(), candidates.end(),
        [](auto const& a, auto const& b) { return a.second < b.second; });
    return it->first;
}
