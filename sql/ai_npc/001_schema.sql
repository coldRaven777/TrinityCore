-- =====================================================================================
-- LivingNPC (Alive NPCs) — Spec 002 schema
-- Dedicated `ai_npc` database for AI event logs + character registry.
-- Keeps ALL AI data out of the game databases (auth/world/characters/hotfixes).
-- Docs: docs/ai-npc/SPEC-002.md §3.4
--
-- Run ONCE (e.g. as MySQL root):
--   mysql -u root < sql/ai_npc/001_schema.sql
-- (the worldserver does NOT auto-create this database; it only connects to it)
-- =====================================================================================

CREATE DATABASE IF NOT EXISTS ai_npc
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE ai_npc;

-- Give the local dev user access (adjust to your own worldserver DB user / host).
GRANT ALL PRIVILEGES ON ai_npc.* TO 'trinity'@'localhost';
FLUSH PRIVILEGES;

-- -------------------------------------------------------------------------------------
-- Per-ENTRY identity / bio metadata. One row per creature_template.entry.
-- The bio CONTENT itself lives in AI/characters/<entry> - <name>.md
-- -------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS ai_character (
  entry        INT UNSIGNED NOT NULL PRIMARY KEY,      -- creature_template.entry
  name         VARCHAR(64)  NOT NULL,                  -- display name (from creature_template at first contact)
  bio_file     VARCHAR(255) NOT NULL,                  -- e.g. "3123 - Wizengamot guard.md"
  enabled      TINYINT(1)   NOT NULL DEFAULT 1,        -- AI participation toggle
  model        VARCHAR(64)  NULL,                      -- per-NPC model override (NULL = global AISystem.ChatModel)
  temperature  FLOAT        NULL,                      -- per-NPC temperature override
  language     VARCHAR(8)   NULL,                      -- per-NPC language override (NULL = AISystem.Language)
  last_seen    DATETIME     NULL,                      -- last event logged for this NPC (dashboard/debug)
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- -------------------------------------------------------------------------------------
-- Per-SPAWN event stream. Each NPC *individual* has its own actor_uid
-- (npc_spawn:<spawn_guid>); the creature entry is carried separately (actor_entry)
-- so the dashboard/bios can join on the identity.
-- -------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS ai_event (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  type         VARCHAR(32) NOT NULL,                   -- npc_say | player_say | npc_emote | death | combat_initiated | combat_ended | npc_evade | player_entered_range
  actor_uid    VARCHAR(64) NOT NULL,                   -- npc_spawn:<spawn_guid> | player:<character_guid>
  actor_entry  INT UNSIGNED NULL,                      -- creature entry for NPC actors (join to ai_character); NULL for players
  actor_name   VARCHAR(64) NULL,                       -- denormalized display name for the dashboard
  target_uid   VARCHAR(64) NULL,                       -- who the event is aimed at (player/npc) or NULL
  target_name  VARCHAR(64) NULL,
  content      TEXT        NULL,                       -- spoken/emoted text or event detail
  location     VARCHAR(128) NULL,                      -- "Zone (map <id>) @ x,y"
  map_id       INT UNSIGNED NULL,
  x            FLOAT NULL, y FLOAT NULL, z FLOAT NULL,
  related      JSON        NULL,                       -- ["npc_spawn:60124", ...] witnesses/participants
  payload      JSON        NULL,                       -- type-specific extras (e.g. {"killer_guid":123})
  ai_processed TINYINT(1)  NOT NULL DEFAULT 0,         -- 0 = logged but NOT fed into any LLM prompt (dashboard RED badge)
  processed_at DATETIME    NULL,                       -- when it was included in a prompt
  created_at   DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  world_time   INT UNSIGNED NULL,                      -- unix time at emit
  INDEX idx_type (type),
  INDEX idx_actor (actor_uid),
  INDEX idx_target (target_uid),
  INDEX idx_created (created_at),
  INDEX idx_entry (actor_entry)
) ENGINE=InnoDB;
