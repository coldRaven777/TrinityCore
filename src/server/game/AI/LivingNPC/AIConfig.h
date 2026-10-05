/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — configuration holder.
 * Reads AISystem.* keys from worldserver.conf via sConfigMgr, cached after Load().
 *
 * NOTE: the real OpenRouter API key must NEVER be committed. It lives only in your
 * local (untracked) worldserver.conf. conf.dist documents the key name with a placeholder.
 */

#ifndef TRINITY_AI_CONFIG_H
#define TRINITY_AI_CONFIG_H

#include "Define.h"
#include <string>
#include <atomic>

class TC_GAME_API AIConfig
{
public:
    static AIConfig* Get();
    void Load();

    bool IsEnabled() const { return m_enabled; }
    bool UseDecisionModel() const { return m_useDecisionModel; }
    bool AmbientNeedsPlayer() const { return m_ambientNeedsPlayer; }

    uint32 MaxTokens() const { return m_maxTokens; }
    uint32 EarshotRadius() const { return m_earshotRadius; }
    uint32 RequestTimeoutSec() const { return m_requestTimeout; }
    uint32 CooldownSec() const { return m_cooldown; }
    uint32 NpcChatCooldownSec() const { return m_npcChatCooldown; }

    // --- Spec 003 ---
    uint32 AmbientGlobalCooldownSec() const { return m_ambientGlobalCooldown; }
    float  ResponderDistanceBias() const { return m_responderDistanceBias; }
    uint32 AmbientMaxPerMinute() const { return m_ambientMaxPerMinute; }

    float Temperature() const { return m_temperature; }
    float ContextSystemRatio() const { return m_contextSystemRatio; }
    float ContextHistoryRatio() const { return m_contextHistoryRatio; }
    float InterjectChance() const { return m_interjectChance; }

    std::string const& OpenRouterApiKey() const { return m_apiKey; }
    std::string const& ChatModel() const { return m_chatModel; }
    std::string const& DecisionModel() const { return m_decisionModel; }
    bool HasValidKey() const { return !m_apiKey.empty(); }

    // --- Spec 002 ---
    bool EventLog() const { return m_eventLog; }
    float ActivationRadius() const { return m_activationRadius; }
    uint32 MaxInjectedEvents() const { return m_maxInjectedEvents; }
    uint32 EventTrimChars() const { return m_eventTrimChars; }
    uint32 SummaryChars() const { return m_summaryChars; }
    std::string const& Language() const { return m_language; }
    std::string const& PromptRoot() const { return m_promptRoot; }
    // Spec 002 fix: only humanoid + mechanical creatures may be AI speakers.
    bool SpeakableOnlyHumanoidMechanical() const { return m_speakableOnlyHumanoidMechanical; }

private:
    AIConfig() = default;
    ~AIConfig() = default;
    AIConfig(AIConfig const&) = delete;
    AIConfig& operator=(AIConfig const&) = delete;

    bool m_enabled = false;
    bool m_useDecisionModel = true;
    bool m_ambientNeedsPlayer = true;

    uint32 m_maxTokens = 1024;
    uint32 m_earshotRadius = 15;
    uint32 m_requestTimeout = 8;
    uint32 m_cooldown = 3;
    uint32 m_npcChatCooldown = 60; // Spec 003: was 50; ambient narrator cadence

    // --- Spec 003 ---
    uint32 m_ambientGlobalCooldown = 30; // min gap between ANY ambient narrator call (all maps)
    float  m_responderDistanceBias = 1.0f; // distance weighting for responder rule-fallback (0 = legacy)
    uint32 m_ambientMaxPerMinute = 0; // 0 = unlimited; else hard burst cap

    float m_temperature = 0.9f;
    float m_contextSystemRatio = 0.35f;
    float m_contextHistoryRatio = 0.55f;
    float m_interjectChance = 0.25f;

    std::string m_apiKey;
    std::string m_chatModel = "openai/gpt-4o-mini";
    std::string m_decisionModel = "anthropic/claude-3-haiku";

    // --- Spec 002 ---
    bool m_eventLog = true;
    float m_activationRadius = 40.0f;
    uint32 m_maxInjectedEvents = 20;
    uint32 m_eventTrimChars = 4000;
    uint32 m_summaryChars = 1200;
    std::string m_language = "en";
    std::string m_promptRoot = "ai";

    // Spec 002 fix: when true, only CREATURE_TYPE_HUMANOID (7) and
    // CREATURE_TYPE_MECHANICAL (9) can be selected as AI speakers.
    bool m_speakableOnlyHumanoidMechanical = true;
};

#define sAIConfig AIConfig::Get()

#endif // TRINITY_AI_CONFIG_H
