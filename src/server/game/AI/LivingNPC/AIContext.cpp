/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIContext.h"

void AIContext::AddTurn(std::string const& key, std::string const& role, std::string const& content)
{
    if (key.empty() || content.empty())
        return;
    std::lock_guard<std::mutex> guard(m_lock);
    auto& hist = m_history[key];
    hist.push_back({ role, content });
    // Bound the stored history to a sane number of turns (safety; BuildMessages trims by tokens).
    while (hist.size() > 40)
        hist.erase(hist.begin());
}

std::vector<AIMessage> AIContext::BuildMessages(std::string const& key,
                                                 std::string const& systemPrompt,
                                                 std::string const& currentUserLine,
                                                 float historyRatio, uint32 maxTokens)
{
    std::vector<AIMessage> messages;
    if (!systemPrompt.empty())
        messages.push_back({ "system", systemPrompt });

    uint32 historyBudget = uint32(maxTokens * historyRatio);

    std::vector<AIMessage> hist;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        auto it = m_history.find(key);
        if (it != m_history.end())
            hist = it->second;
    }

    // Trim oldest-first until history fits the budget (system + history must stay under maxTokens).
    uint32 used = EstimateTokens(systemPrompt) + EstimateTokens(currentUserLine);
    size_t start = 0;
    while (start < hist.size())
    {
        uint32 add = 0;
        for (size_t i = start; i < hist.size(); ++i)
            add += EstimateTokens(hist[i].role) + EstimateTokens(hist[i].content);
        if (used + add <= historyBudget || start == hist.size() - 1)
            break;
        start++;
    }
    for (size_t i = start; i < hist.size(); ++i)
        messages.push_back(hist[i]);

    if (!currentUserLine.empty())
        messages.push_back({ "user", currentUserLine });

    return messages;
}

std::string AIContext::GetSummary(std::string const& key, uint32 maxChars)
{
    std::vector<AIMessage> hist;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        auto it = m_history.find(key);
        if (it != m_history.end())
            hist = it->second;
    }
    if (hist.empty() || maxChars == 0)
        return "";

    // Walk newest-first; build the recap so the OLDEST turns that still fit come first.
    std::string out;
    for (auto it = hist.rbegin(); it != hist.rend(); ++it)
    {
        std::string who = (it->role == "user") ? "Speaker" : "You";
        std::string line = who + ": " + it->content;
        if (out.size() + line.size() + 1 > maxChars)
            break;
        if (!out.empty())
            line += "\n";
        out = line + out;
    }
    return out;
}
