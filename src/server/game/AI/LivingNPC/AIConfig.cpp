/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIConfig.h"
#include "Config.h"
#include "World.h"

AIConfig* AIConfig::Get()
{
    static AIConfig instance;
    return &instance;
}

void AIConfig::Load()
{
    m_enabled          = sConfigMgr->GetBoolDefault("AISystem.Enabled", false);
    m_useDecisionModel = sConfigMgr->GetBoolDefault("AISystem.UseDecisionModel", true);
    m_ambientNeedsPlayer = sConfigMgr->GetBoolDefault("AISystem.AmbientNeedsPlayer", true);

    m_maxTokens        = sConfigMgr->GetIntDefault("AISystem.MaxTokens", 1024);
    m_earshotRadius    = sConfigMgr->GetIntDefault("AISystem.EarshotRadius", 15);
    m_requestTimeout   = sConfigMgr->GetIntDefault("AISystem.RequestTimeoutSec", 8);
    m_cooldown         = sConfigMgr->GetIntDefault("AISystem.CooldownSec", 3);
    m_npcChatCooldown  = sConfigMgr->GetIntDefault("AISystem.NpcChatCooldownSec", 60); // Spec 003: was 50

    // --- Spec 003 ---
    m_ambientGlobalCooldown = sConfigMgr->GetIntDefault("AISystem.AmbientGlobalCooldownSec", 30);
    m_responderDistanceBias = sConfigMgr->GetFloatDefault("AISystem.ResponderDistanceBias", 1.0f);
    m_ambientMaxPerMinute   = sConfigMgr->GetIntDefault("AISystem.AmbientMaxPerMinute", 0);

    m_temperature          = sConfigMgr->GetFloatDefault("AISystem.Temperature", 0.9f);
    m_contextSystemRatio  = sConfigMgr->GetFloatDefault("AISystem.ContextSystemRatio", 0.35f);
    m_contextHistoryRatio = sConfigMgr->GetFloatDefault("AISystem.ContextHistoryRatio", 0.55f);
    m_interjectChance      = sConfigMgr->GetFloatDefault("AISystem.InterjectChance", 0.25f);

    m_apiKey       = sConfigMgr->GetStringDefault("AISystem.OpenRouterApiKey", "");
    m_chatModel    = sConfigMgr->GetStringDefault("AISystem.ChatModel", "openai/gpt-4o-mini");
    m_decisionModel= sConfigMgr->GetStringDefault("AISystem.DecisionModel", "anthropic/claude-3-haiku");

    // --- Spec 002 ---
    m_eventLog         = sConfigMgr->GetBoolDefault("AISystem.EventLog", true);
    m_activationRadius = sConfigMgr->GetFloatDefault("AISystem.ActivationRadius", 40.0f);
    m_maxInjectedEvents= sConfigMgr->GetIntDefault("AISystem.MaxInjectedEvents", 20);
    m_eventTrimChars   = sConfigMgr->GetIntDefault("AISystem.EventTrimChars", 4000);
    m_summaryChars     = sConfigMgr->GetIntDefault("AISystem.SummaryChars", 1200);
    m_language         = sConfigMgr->GetStringDefault("AISystem.Language", "en");
    m_promptRoot       = sConfigMgr->GetStringDefault("AISystem.PromptRoot", "ai");
    m_speakableOnlyHumanoidMechanical = sConfigMgr->GetBoolDefault("AISystem.SpeakableOnlyHumanoidMechanical", true);

    if (m_enabled && m_apiKey.empty())
        TC_LOG_ERROR("AISystem", "AISystem.Enabled=1 but AISystem.OpenRouterApiKey is empty; disabling LLM calls.");
}
