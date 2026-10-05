/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIEvent.h"
#include "AI/LivingNPC/AIConfig.h"

#include "DatabaseEnv.h"
#include "Config.h"
#include "Map.h"
#include "Player.h"
#include "Creature.h"
#include "Unit.h"
#include "GameTime.h"
#include "Log.h"
#include "Util.h"
#include "StringFormat.h"

#include <cmath>

namespace AIEvent
{
    namespace
    {
        bool g_poolOpen = false;

        std::string UidForPlayer(Player* p)
        {
            return "player:" + std::to_string(p->GetGUID().GetCounter());
        }

        std::string UidForNpc(Creature* c)
        {
            return "npc_spawn:" + std::to_string(uint64(c->GetSpawnId()));
        }

        std::string JsonEscape(std::string const& s)
        {
            std::string out;
            out.reserve(s.size() + 8);
            for (char ch : s)
            {
                switch (ch)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:   out += ch;     break;
                }
            }
            return out;
        }

        // Build a JSON array of entity uids for the `related` column.
        std::string RelatedJson(std::vector<std::string> const& related)
        {
            std::string out = "[";
            for (size_t i = 0; i < related.size(); ++i)
            {
                if (i) out += ",";
                out += "\"" + JsonEscape(related[i]) + "\"";
            }
            out += "]";
            return out;
        }

        std::string LocationFor(WorldObject const* obj)
        {
            if (!obj)
                return "";
            Map* map = obj->GetMap();
            if (!map)
                return "";
            return Trinity::StringFormat("{} (map {}) @ {}, {}", map->GetMapName(), map->GetId(),
                std::to_string(obj->GetPositionX()), std::to_string(obj->GetPositionY()));
        }

        std::vector<Creature*> CollectNearbyCreatures(WorldObject const* center, float radius)
        {
            std::vector<Creature*> out;
            if (!center)
                return out;
            Map* map = center->GetMap();
            if (!map)
                return out;
            for (auto const& kv : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = kv.second;
                if (!c || !c->IsAlive() || c == center)
                    continue;
                if (center->GetDistance(c) <= radius)
                    out.push_back(c);
            }
            return out;
        }
    }

    char const* TypeName(Type type)
    {
        switch (type)
        {
            case Type::NpcSay:             return "npc_say";
            case Type::PlayerSay:          return "player_say";
            case Type::NpcEmote:           return "npc_emote";
            case Type::Death:              return "death";
            case Type::CombatInitiated:    return "combat_initiated";
            case Type::CombatEnded:        return "combat_ended";
            case Type::NpcEvade:           return "npc_evade";
            case Type::PlayerEnteredRange: return "player_entered_range";
            default:                       return "unknown";
        }
    }

    void Initialize()
    {
        std::string const dbString = sConfigMgr->GetStringDefault("AIDatabaseInfo", "");
        if (dbString.empty())
        {
            TC_LOG_INFO("AISystem", "AI event log: AIDatabaseInfo not set; event logging disabled.");
            g_poolOpen = false;
            return;
        }

        AIDatabase.SetConnectionInfo(dbString, 1, 1);
        if (uint32 error = AIDatabase.Open())
        {
            TC_LOG_ERROR("AISystem", "AI event log: could not open the 'ai_npc' database (error {}). "
                "Event logging disabled.", error);
            g_poolOpen = false;
            return;
        }

        g_poolOpen = true;
        TC_LOG_INFO("AISystem", "AI event log: connected to 'ai_npc' (dedicated AIDatabase pool).");
    }

    void Shutdown()
    {
        if (g_poolOpen)
        {
            AIDatabase.Close();
            g_poolOpen = false;
        }
    }

    bool IsReady()
    {
        return g_poolOpen && sAIConfig->EventLog();
    }

    void Reload()
    {
        // EventLog flag is re-read from sAIConfig on every IsReady() call already.
    }

    bool PlayerNear(WorldObject const* obj, float radius)
    {
        if (!obj)
            return false;
        Map* map = obj->GetMap();
        if (!map)
            return false;
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (p && p->IsInWorld() && p->IsAlive())
                if (obj->GetDistance(p) <= radius)
                    return true;
        }
        return false;
    }

    void LogEvent(Type type,
             std::string const& actorUid, std::string const& actorName, uint32 actorEntry,
             std::string const& targetUid, std::string const& targetName,
             std::string const& content,
             uint32 mapId, float x, float y, float z,
             std::string const& location,
             std::vector<std::string> const& related,
             std::string const& payloadJson)
    {
        if (!IsReady())
            return;

        std::string aUid = actorUid, aName = actorName, tUid = targetUid, tName = targetName;
        std::string c = content, loc = location, rel = RelatedJson(related), pay = payloadJson;
        AIDatabase.EscapeString(aUid);
        AIDatabase.EscapeString(aName);
        AIDatabase.EscapeString(tUid);
        AIDatabase.EscapeString(tName);
        AIDatabase.EscapeString(c);
        AIDatabase.EscapeString(loc);
        AIDatabase.EscapeString(rel);
        AIDatabase.EscapeString(pay);

        std::string sql = "INSERT INTO ai_event (type, actor_uid, actor_entry, actor_name, target_uid, "
            "target_name, content, location, map_id, x, y, z, related, payload, world_time) VALUES ('";
        sql += TypeName(type);
        sql += "', '" + aUid + "', " + std::to_string(actorEntry) + ", '" + aName + "', '" + tUid + "', '" + tName + "', '";
        sql += c;
        sql += "', '" + loc + "', " + std::to_string(mapId) + ", " + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ", '";
        sql += rel;
        sql += "', '" + pay + "', " + std::to_string(GameTime::GetGameTime()) + ")";

        AIDatabase.Execute(sql.c_str());
    }

    void LogNpcSay(Creature* c, std::string const& text, std::string const& targetUid, std::string const& targetName)
    {
        if (!c || text.empty() || !IsReady())
            return;
        if (!PlayerNear(c, sAIConfig->ActivationRadius()))
            return;

        std::vector<std::string> related;
        for (Creature* o : CollectNearbyCreatures(c, sAIConfig->EarshotRadius()))
            related.push_back(UidForNpc(o));

        LogEvent(Type::NpcSay, UidForNpc(c), c->GetName(), c->GetEntry(),
            targetUid, targetName, text,
            c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(),
            LocationFor(c), related, "{}");
    }

    void LogPlayerSay(Player* p, std::string const& text)
    {
        if (!p || text.empty() || !IsReady())
            return;

        std::vector<std::string> related;
        for (Creature* o : CollectNearbyCreatures(p, sAIConfig->EarshotRadius()))
            related.push_back(UidForNpc(o));

        LogEvent(Type::PlayerSay, UidForPlayer(p), p->GetName(), 0,
            "", "", text,
            p->GetMapId(), p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(),
            LocationFor(p), related, "{}");
    }

    void LogNpcEmote(Creature* c, std::string const& emote)
    {
        if (!c || emote.empty() || !IsReady())
            return;
        if (!PlayerNear(c, sAIConfig->ActivationRadius()))
            return;

        LogEvent(Type::NpcEmote, UidForNpc(c), c->GetName(), c->GetEntry(),
            "", "", emote,
            c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(),
            LocationFor(c), {}, "{}");
    }

    void LogDeath(Unit* victim, Unit* killer)
    {
        if (!victim || !IsReady())
            return;
        Creature* dead = victim->ToCreature();
        if (!dead)
            return; // only creature deaths are logged for now
        if (!PlayerNear(victim, sAIConfig->ActivationRadius()))
            return;

        std::string targetUid, targetName;
        uint32 killerGuid = 0;
        if (killer)
        {
            if (Player* kp = killer->ToPlayer())
            {
                targetUid = UidForPlayer(kp);
                targetName = kp->GetName();
            }
            else if (Creature* kc = killer->ToCreature())
            {
                targetUid = UidForNpc(kc);
                targetName = kc->GetName();
            }
            killerGuid = killer->GetGUID().GetCounter();
        }

        std::vector<std::string> related;
        for (Creature* o : CollectNearbyCreatures(victim, sAIConfig->EarshotRadius()))
            related.push_back(UidForNpc(o));

        std::string payload = "{\"killer_guid\":" + std::to_string(killerGuid) + "}";
        LogEvent(Type::Death, UidForNpc(dead), dead->GetName(), dead->GetEntry(),
            targetUid, targetName, "",
            dead->GetMapId(), dead->GetPositionX(), dead->GetPositionY(), dead->GetPositionZ(),
            LocationFor(dead), related, payload);
    }

    void LogCombatStart(Unit* owner, Unit* enemy)
    {
        if (!owner || !enemy || !IsReady())
            return;
        Creature* c = owner->ToCreature();
        if (!c)
            return; // only NPCs are logged as combat actors for now
        if (!PlayerNear(c, sAIConfig->ActivationRadius()))
            return;

        std::string targetUid, targetName;
        if (Player* ep = enemy->ToPlayer())
        {
            targetUid = UidForPlayer(ep);
            targetName = ep->GetName();
        }
        else if (Creature* ec = enemy->ToCreature())
        {
            targetUid = UidForNpc(ec);
            targetName = ec->GetName();
        }

        std::vector<std::string> related;
        for (Creature* o : CollectNearbyCreatures(c, sAIConfig->EarshotRadius()))
            related.push_back(UidForNpc(o));

        LogEvent(Type::CombatInitiated, UidForNpc(c), c->GetName(), c->GetEntry(),
            targetUid, targetName, "",
            c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(),
            LocationFor(c), related, "{}");
    }

    void LogCombatEnd(Unit* owner)
    {
        if (!owner || !IsReady())
            return;
        Creature* c = owner->ToCreature();
        if (!c)
            return;
        if (!PlayerNear(c, sAIConfig->ActivationRadius()))
            return;

        std::vector<std::string> related;
        for (Creature* o : CollectNearbyCreatures(c, sAIConfig->EarshotRadius()))
            related.push_back(UidForNpc(o));

        LogEvent(Type::CombatEnded, UidForNpc(c), c->GetName(), c->GetEntry(),
            "", "", "",
            c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(),
            LocationFor(c), related, "{}");
    }

    void LogEvade(Unit* owner)
    {
        if (!owner || !IsReady())
            return;
        Creature* c = owner->ToCreature();
        if (!c)
            return;
        if (!PlayerNear(c, sAIConfig->ActivationRadius()))
            return;

        LogEvent(Type::NpcEvade, UidForNpc(c), c->GetName(), c->GetEntry(),
            "", "", "",
            c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(),
            LocationFor(c), {}, "{}");
    }

    std::string RecentEventsFor(uint32 mapId, std::vector<std::string> const& involvedUids,
                                uint32 maxRows, uint32 charCap)
    {
        if (!IsReady() || involvedUids.empty() || maxRows == 0)
            return "";

        // Build IN lists (escaped) + JSON_CONTAINS matches for the `related` column.
        std::string inList, contains;
        for (size_t i = 0; i < involvedUids.size(); ++i)
        {
            std::string u = involvedUids[i];
            AIDatabase.EscapeString(u);
            if (i) { inList += ","; contains += " OR "; }
            inList += "'" + u + "'";
            // JSON_CONTAINS(related, '"uid"')
            contains += "JSON_CONTAINS(related, '\"" + u + "\"')";
        }

        std::string sql = "SELECT id, type, actor_name, content, DATE_FORMAT(created_at, '%H:%i') "
            "FROM ai_event WHERE map_id = " + std::to_string(mapId) +
            " AND (actor_uid IN (" + inList + ") OR target_uid IN (" + inList + ") OR " + contains + ") "
            "ORDER BY id DESC LIMIT " + std::to_string(maxRows);

        QueryResult result = AIDatabase.Query(sql.c_str());
        if (!result)
            return "";

        std::string out;
        std::string ids;
        out.reserve(1024);
        do
        {
            Field* fields = result->Fetch();
            uint64 id = fields[0].GetUInt64();
            std::string type = fields[1].GetString();
            std::string name = fields[2].GetString();
            std::string content = fields[3].GetString();
            std::string time = fields[4].GetString();

            if (!ids.empty()) ids += ",";
            ids += std::to_string(id);

            std::string line;
            if (type == "npc_say" || type == "player_say")
                line = "[" + time + "] " + name + ": " + content;
            else
                line = "[" + time + "] " + name + " <" + type + ">" + (content.empty() ? "" : " " + content);

            if (out.size() + line.size() + 1 > charCap)
                break; // stop; order is newest-first so the tail is the oldest — fine to drop
            if (!out.empty()) out += "\n";
            out += line;
        } while (result->NextRow());

        // Mark the injected rows as processed so the dashboard can tell they reached the AI.
        if (!ids.empty())
            AIDatabase.PExecute("UPDATE ai_event SET ai_processed = 1, processed_at = NOW() WHERE id IN ({})", ids);

        return out;
    }
}
