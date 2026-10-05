/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — chat commands.
 *   aisai "text"        Force the selected NPC to say text (no LLM; mirrors .npc say).
 *   ai on|off           Enable/disable LivingNPC for your session.
 *
 * 12.x command API: GetCommands() returns std::span<ChatCommandBuilder const>,
 * tables are `ChatCommandTable` (= ChatCommandBuilder const[]).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "RBAC.h"
#include "Creature.h"
#include "Player.h"
#include "Language.h"
#include "AI/LivingNPC/AIManager.h"
#include "AI/LivingNPC/AIPrompt.h"
#include "AI/LivingNPC/AIEvent.h"

#include <span>

using namespace Trinity::ChatCommands;

class ai_commandscript : public CommandScript
{
public:
    ai_commandscript() : CommandScript("ai_commandscript") { }

    static bool HandleAisaiCommand(ChatHandler* handler, Tail text)
    {
        if (text.empty())
        {
            handler->SendSysMessage("Usage: .aisai <text> (select an NPC first)");
            return false;
        }

        Creature* creature = nullptr;
        if (Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr)
        {
            if (Unit* selected = player->GetSelectedUnit())
                creature = selected->ToCreature();
        }
        if (!creature)
        {
            handler->SendSysMessage("You must select an NPC first.");
            return false;
        }

        // Mirror .npc say: just make the NPC speak the text, no LLM.
        creature->Say(std::string(text), LANG_UNIVERSAL);
        return true;
    }

    static bool HandleAiOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        uint32 account = (player && player->GetSession()) ? player->GetSession()->GetAccountId() : 0;
        sAIManager->SetSessionEnabled(account, true);
        handler->SendSysMessage("LivingNPC enabled for your session.");
        return true;
    }

    static bool HandleAiOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        uint32 account = (player && player->GetSession()) ? player->GetSession()->GetAccountId() : 0;
        sAIManager->SetSessionEnabled(account, false);
        handler->SendSysMessage("LivingNPC disabled for your session.");
        return true;
    }

    static bool HandleAiReloadCommand(ChatHandler* handler)
    {
        AIPrompt::Reload();   // flush template cache + re-index bios
        AIEvent::Reload();    // re-check event-log flag
        handler->SendSysMessage("LivingNPC: prompt cache reloaded (templates + bios).");
        return true;
    }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable aiSubTable =
        {
            { "on",     HandleAiOnCommand,     rbac::RBAC_PERM_COMMAND_NPC_INFO, Console::Yes },
            { "off",    HandleAiOffCommand,    rbac::RBAC_PERM_COMMAND_NPC_INFO, Console::Yes },
            { "reload", HandleAiReloadCommand, rbac::RBAC_PERM_COMMAND_NPC_SAY,  Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "aisai", HandleAisaiCommand, rbac::RBAC_PERM_COMMAND_NPC_SAY, Console::No },
            { "ai", aiSubTable },
        };

        return commandTable;
    }
};

void AddSC_ai_commandscript()
{
    new ai_commandscript();
}
