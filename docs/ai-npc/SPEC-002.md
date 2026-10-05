# Spec 002 — File-driven prompts, Event log, and Dashboard

**Depends on:** PROJECT.md §2–§8, SPEC-001 (implemented), IMPLEMENTATION.md (as-built).
**Branch:** `feature/ai-npc`

**Scope (this spec only):**
1. Configurable LLM language — **default English**, changeable in config (supersedes the
   hardcoded `ptBR` in `LocaleFor()`, IMPLEMENTATION.md §9.3).
2. Fully exposed, **file-driven prompt system**: every prompt a request can receive is a
   plain `.md` file under `AI/prompts/<type>/`, editable with any text editor, no recompile.
   NPC bios live in `AI/characters/<entry> - <name>.md`, auto-created if missing.
3. **Event system**: schema + async logging for say / emote / death / combat / presence,
   **keyed by spawn GUID** and **gated by player proximity**. Events are the substrate the
   AI will later inject from (based on involved entities) and that memory (Spec 003) will
   vectorize. This spec delivers logging + storage + recent-event injection into prompts —
   **no memory yet**.
4. `ai reload` command; prompt/bio cache with mtime invalidation so edits are hot-reloaded.
5. **WebUI dashboard** (separate FastAPI sidecar): monitor events, add/edit NPCs & bios,
   edit prompts, view config.

**Out of scope (later specs):** long-term memory + vectorization (Spec 003), knowledge
injection (Spec 004), quest hooks, timed group chat.

> **Roadmap renumber:** PROJECT.md §10 listed "Spec 2 — Memory". Memory + vectorization is
> now **Spec 003**. This spec (002) is the prompt/event/dashboard foundation memory builds on.

---

## 1. Language

The LLM's output language is a first-class config value, defaulting to English.

```ini
AISystem.Language = en        # ISO 639-1 (lowercase). Default English.
```

- Resolution order: `ai_character.language` (per-NPC override) → `AISystem.Language`.
- Replaces the hardcoded `LocaleFor()` → `"ptBR"` in `AIManager` (IMPLEMENTATION.md §9.3).
- The resolved language is:
  - injected into templates as `{{language}}`, and
  - woven into the default system prompt (see §2.4).
- Future (Spec 004): per-player inference from client locale; not now.

---

## 2. Prompt system — file driven

### 2.1 Principle

> **No prompt text lives in C++.** Every prompt a request can receive is a set of `.md`
> files on disk. The engine only *assembles* files + fills `{{placeholders}}`.

A prompt for a request is assembled from **slots**. Each request *type* (player chats with
NPC, NPC replies to NPC, ambient narrator, …) declares its own slot combination via a
`slots.txt` manifest. Slots fall back to `AI/prompts/shared/`.

### 2.2 Folder layout

```
<AISystem.PromptRoot>/            # default: "ai" relative to worldserver working dir (absolute ok)
├── prompts/
│   ├── shared/                   # fallback slots used by ANY type
│   │   ├── system.md
│   │   └── reinforcement.md
│   ├── player_chat/              # player speaks → selected/front NPC answers
│   │   ├── slots.txt
│   │   ├── system.md             # identity + world rules + tone
│   │   ├── bio.md                # slot {{bio}}  → NPC's bio file (per ENTRY)
│   │   ├── memories.md           # slot {{memories}} → EMPTY (Spec 003 fills it)
│   │   ├── summary.md            # slot {{summary}} → naive recap of THIS SPAWN's conversation
│   │   ├── events.md             # slot {{events}}  → recent ai_event rows involving these entities
│   │   └── reinforcement.md      # "Stay in character as {{npc}} ... time {{time}} ... location {{location}}"
│   ├── npc_chat/                 # NPC speaks → another NPC responds (ambient)
│   │   └── (same slot files, tuned copy)
│   └── ambient/                  # narrator picks an NPC to speak spontaneously
│       └── (same slot files, tuned copy)
└── characters/                   # one .md per NPC IDENTITY (per creature entry)
    ├── 3123 - Wizengamot guard.md
    └── ...
```

`slots.txt` — assembly order + which message role each file becomes:

```
# <slot-file> : <role>     role = system | user
system.md        : system
bio.md           : user
memories.md      : user
summary.md       : user
events.md        : user
reinforcement.md : user
```

If a type folder has no `slots.txt`, the default order is exactly the six lines above.
Missing files are skipped silently; missing **shared** files fall back to a small built-in
system prompt (last-resort safety net only — the folder is the source of truth).

### 2.3 Placeholders

| Placeholder | Resolved to |
|---|---|
| `{{npc}}` | NPC display name |
| `{{npc_entry}}` | creature_template.entry (the "entity id") |
| `{{player}}` | responding/calling player's name |
| `{{target}}` | the addressee's name (player or NPC) |
| `{{zone}}`, `{{map}}` | current zone name / map name |
| `{{location}}` | `"<Zone> (map <id>) @ <x>,<y>"` |
| `{{time}}` | current server time, `HH:MM` |
| `{{language}}` | resolved response language (§1) |
| `{{last_line}}` | the line being answered (player line / prior NPC line) |
| `{{bio}}` `{{memories}}` `{{summary}}` `{{events}}` | injected slot content |

Unresolved placeholders are **left verbatim** in the output (so a typo is visible to the
author) and a one-time `AISystem` warning is logged per template+placeholder pair.

### 2.4 Built-in default system prompt (only if `shared/system.md` missing)

```
You are {{npc}}, a creature in the world of Warcraft.
Stay fully in character. Never mention that you are an AI or break character.
Respond naturally and concisely (one or two sentences).
Always reply in the language '{{language}}'.
```

### 2.5 Loading / hot reload

- `AIPrompt` keeps a cache: `path → (mtime, parsed text)`. On every assembly it stats the
  files (cheap); any mtime change re-reads. So editing a `.md` from the WebUI or an editor
  is picked up by the **next request** — no restart.
- `.ai reload` forces a full cache flush + re-index of `characters/` (see §4.4).
- On first boot with no `ai/` folder: the module **seeds** `shared/`, `player_chat/`,
  `npc_chat/`, `ambient/` with the default templates (including the §2.4 default), so the
  system is fully exposed out of the box.

### 2.6 Identity model — bio per ENTRY, events/summaries per SPAWN

Two distinct dimensions (this split is deliberate):

| What | Keyed by | Stored in |
|---|---|---|
| **Bio / persona** (who this kind of NPC is) | creature **entry** | `AI/characters/<entry> - <name>.md` + `ai_character` |
| **Events** (what this *individual* did) | **spawn GUID** | `ai_event` (§3) |
| **Conversation summary** (what this *individual* talked about) | **spawn GUID** | in-context `AIContext` key (already keys by spawn GUID) — persistent form in Spec 003 |

- Bio files: one per **entry**, named `**<entry> - <name>.md**` (e.g.
  `3123 - Wizengamot guard.md`). `<entry>` is `creature_template.entry`; `<name>` is the
  display name (from DB at first contact). All spawns of entry 3123 share the same bio.
- Filename sanitization on create: strip `\ / : * ? " < > |` and trim/collapse spaces —
  the entry number is the key, the name is for humans.
- Lookup by **entry**: `AIPrompt` keeps an in-memory index of `entry → file path`, rebuilt
  on `.ai reload` / mtime scan. Parse the leading integer of each filename.
- **Auto-create:** if no file matches the entry on first contact, create an empty one
  (`<entry> - <name>.md` with a small header comment) and index it. Content goes into
  `{{bio}}`. Empty bio → empty `{{bio}}` (nothing breaks).
- The same event also upserts the NPC into `ai_character` (§3.4) so the dashboard can list
  every NPC that has ever participated.
- **Events and summaries NEVER use the entry as the event's identity** — they are keyed by
  spawn GUID, so each individual guard of entry 3123 has its own event stream and summary.

---

## 3. Event system

### 3.1 What an event is

A single, immutable fact about the world that the AI cares about: someone spoke, someone
died, combat started, a player arrived. Events are the shared substrate for prompt
injection (this spec) and memory/vectorization (Spec 003). Shape (user's example,
normalized — UID is spawn-based for NPCs):

```json
{
  "id": 123,                        // auto-increment BIGINT (string in the example was illustrative)
  "type": "npc_say",                // see §3.3
  "actor_uid": "npc_spawn:60123",   // WHO it was (spawn GUID for NPCs)
  "actor_entry": 3123,              // WHAT kind it was (join to bio / ai_character)
  "actor_name": "Wizengamot Guard",
  "target_uid": "player:1000234",   // who it was aimed at (nullable)
  "content": "Halt! State your business.",
  "location": "Stormwind City (map 0) @ 1234.5, 567.8",
  "related_entities": ["npc_spawn:60124", "npc_spawn:60125"],  // who else was involved / may have heard
  "ai_processed": true,             // was this event fed into an LLM prompt? (dashboard red flag when false)
  "created_at": "2026-10-04 20:58:00"
}
```

### 3.2 Entity UIDs

| Kind | UID | Meaning |
|---|---|---|
| NPC | `npc_spawn:<spawn_guid>` | **this individual creature** (per spawn — events & summaries are per spawn) |
| Player | `player:<character_guid>` | per character |

The **entry** is carried as a separate column (`actor_entry`), never part of the event's
identity — it only links the event back to the bio/`ai_character` row (which is per entry).

### 3.3 Event types (v1)

| type | actor | target | content | related | payload (extras) |
|---|---|---|---|---|---|
| `npc_say` | npc | addressee (nullable) | spoken text | earshot NPCs | — |
| `player_say` | player | nullable | spoken text | earshot NPCs | — |
| `npc_emote` | npc | nullable | `*nods*` | earshot NPCs | — |
| `death` | npc (dead) | killer (npc or player) | nullable | nearby creatures | `killer_guid` |
| `combat_initiated` | npc | aggro target | nullable | other combatants | — |
| `combat_ended` | npc | last target | nullable | participants | `outcome` (`won`/`fled`/`evaded`) |
| `npc_evade` | npc | nullable | nullable | nearby | — |
| `player_entered_range` | player | npc (closest) | nullable | earshot NPCs | — |

Stretch (hooks exist but wiring is more work — OK to defer): `quest_accept`,
`quest_complete`, `player_left_range`, `npc_greeting` (player opens gossip).

### 3.4 Storage — decision

**Decided: a separate `ai_npc` schema on the existing MySQL server, reached through a new
`AIDatabase` connection pool.** SQL in `sql/ai_npc/001_schema.sql`:

```sql
CREATE DATABASE IF NOT EXISTS ai_npc DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE ai_npc.ai_character (                    -- per ENTRY (identity / bio metadata)
  entry        INT UNSIGNED NOT NULL PRIMARY KEY,      -- creature_template.entry
  name         VARCHAR(64)  NOT NULL,
  bio_file     VARCHAR(255) NOT NULL,                  -- "3123 - Wizengamot guard.md"
  enabled      TINYINT(1)   NOT NULL DEFAULT 1,
  model        VARCHAR(64)  NULL,                      -- per-NPC model override (NULL = global)
  temperature  FLOAT        NULL,
  language     VARCHAR(8)   NULL,                      -- per-NPC language override (NULL = global)
  last_seen    DATETIME     NULL,
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE ai_npc.ai_event (                        -- per SPAWN (each individual's stream)
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  type         VARCHAR(32) NOT NULL,                   -- npc_say | death | combat_initiated | ...
  actor_uid    VARCHAR(64) NOT NULL,                   -- npc_spawn:<spawn_guid> | player:<guid>
  actor_entry  INT UNSIGNED NULL,                      -- creature entry for NPC actors (join to bio); NULL for players
  actor_name   VARCHAR(64) NULL,
  target_uid   VARCHAR(64) NULL,
  target_name  VARCHAR(64) NULL,
  content      TEXT NULL,                              -- the spoken/emoted text or event detail
  location     VARCHAR(128) NULL,                      -- "Zone (map <id>) @ x,y"
  map_id       INT UNSIGNED NULL,
  x FLOAT NULL, y FLOAT NULL, z FLOAT NULL,
  related      JSON NULL,                              -- ["npc_spawn:60124",...]
  payload      JSON NULL,                              -- type-specific extras
  ai_processed TINYINT(1)   NOT NULL DEFAULT 0,        -- 0 = logged but NOT fed to any LLM prompt (dashboard RED)
  processed_at DATETIME     NULL,                      -- when it was included in a prompt
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  world_time   INT UNSIGNED NULL,
  INDEX idx_type (type), INDEX idx_actor (actor_uid),
  INDEX idx_target (target_uid), INDEX idx_created (created_at),
  INDEX idx_entry (actor_entry)
) ENGINE=InnoDB;
```

Connection config (worldserver.conf, `[connections]` section, alongside the existing
`WorldDatabaseInfo` etc.):

```ini
AIDatabaseInfo = "127.0.0.1;3306;trinity;trinity;ai_npc"
```

### 3.5 Why this storage choice (deliberation — resolved)

| Option | Verdict |
|---|---|
| Table in TC's existing `world` DB | ❌ — pollutes the git-managed game DB, gets clobbered by upstream SQL merges; semantically wrong |
| **Separate `ai_npc` schema on the SAME MySQL server, new `AIDatabase` pool** | ✅ **DECIDED** — zero new infra (MySQL already runs, `trinity` user exists); keeps world DB clean; idiomatic — TC already runs 3 pools (login/char/world), a 4th is the same pattern; worldserver + dashboard read/write it; clean migration path to vectorization |
| SQLite file | ❌ — TC has no SQLite access layer, concurrent-writer headaches |
| External dedicated DB (Postgres + pgvector / vector DB) | ❌ for now — heavy infra today, no TC access layer |

**Vectorization path (Spec 003):** `ai_event` is the **source of truth**. Vector search is a
*derived* concern — Spec 003 adds either a vector column/table in `ai_npc` or exports to a
vector store. Because the canonical events are normalized here, we are not locked into any
choice by this spec.

### 3.6 Emission — async + gated by player proximity

**Rule (applies to EVERY event type and EVERY AI trigger):** the engine only operates in
the **immediate periphery of a player**. An event is logged **only if a player is online
and within `AISystem.ActivationRadius` of the actor**; an AI reply/narrator/interjection
only fires when a player is within that radius. If no player is near, **nothing** is logged
or processed — the world simply runs silently. This supersedes any "AI-participant-only"
noise guard: when a player IS near, events trigger for **every** NPC or player in the
periphery, not just registered AI NPCs.

- `AISystem.ActivationRadius` (default `40.0` yards) = "immediate periphery". Distinct from
  `EarshotRadius` (who *hears*/responds, 15 yards). Configurable.
- Logging is **async**: `AIEvent::Log(...)` builds a `PreparedStatement` and calls
  `AIDatabase->Execute(stmt)` — fire-and-forget through the async DB worker threads. DB
  down / slow → logged error, no crash, no game freeze.
- Event logging is **independent of `AISystem.Enabled`** (so the dashboard has data even
  with AI replies off), but still gated by player proximity.

Emission hook sites (candidates; confirm during implementation):

| Event | Hook |
|---|---|
| `player_say` | `AIManager::OnPlayerSpeak` (already wired in `ChatHandler.cpp`) |
| `npc_say`, `npc_emote` | `AIManager::OnNpcSpeak` (`.npc say` + anything routed through it) **and** the `World::Update` result drain when an LLM reply is actually `Creature::Say`-ed |
| `death` | `Unit::Kill` / `Creature` death path (gate: player within `ActivationRadius` of the dead creature) |
| `combat_initiated`, `combat_ended`, `npc_evade` | `Unit::SetInCombatWith` / combat-stop / `Evade` paths (same gate) |

### 3.7 Event injection into prompts

For a request, `AIPrompt` fills `{{events}}` with the most recent `ai_event` rows whose
`actor_uid`/`target_uid`/`related` touches **any involved entity** (the speaking NPC's
spawn, the addressee player/NPC, or their related spawns), newest-first, formatted as:

```
[20:58] Wizengamot Guard: "Halt! State your business."
[20:59] <Player>: "Who are you?"
[21:01] Wizengamot Guard: "I ask the questions here."
```

- Trim to fit `AISystem.MaxInjectedEvents` (default 20) and a char cap
  (`AISystem.EventTrimChars`, default 4000). No vectorization, no semantic ranking — Spec 003.
- **`ai_processed` marking:** every row actually included in a prompt is marked
  `ai_processed = 1` (+ `processed_at`) in the same async insert/update batch. Rows that
  were logged but never fed to the LLM (e.g. AI disabled, or simply outside an injection
  window) keep `ai_processed = 0` → the dashboard shows them with a **red marker** ("not
  included in AI processing").

---

## 4. Config additions (full list)

```ini
# --- LivingNPC / Spec 002 ---
AISystem.Language            = en            # ISO 639-1; default English. Per-NPC override in ai_character
AISystem.PromptRoot          = "ai"          # root of the AI/ tree (relative to working dir, or absolute)
AISystem.EventLog            = 1             # master switch for event logging (still gated by player proximity)
AISystem.ActivationRadius    = 40.0          # yards; the "immediate periphery" — engine + events active only here
AISystem.MaxInjectedEvents   = 20            # recent events injected into {{events}}
AISystem.EventTrimChars      = 4000          # char cap for {{events}}
AISystem.SummaryChars        = 1200          # char cap for {{summary}} (naive, this spec)

# [connections]
AIDatabaseInfo = "127.0.0.1;3306;trinity;trinity;ai_npc"
```

`AISystem.Enabled` and the Spec 001 keys are unchanged.

---

## 5. WebUI dashboard (separate FastAPI sidecar)

**Decided: a standalone local web app** — FastAPI (chosen: most efficient to implement,
easy to iterate; a single small service + a single-page HTML frontend) — running on
`127.0.0.1`, talking to the **same `ai_npc` schema** and reading/writing the **same `AI/`
folder**. The worldserver needs *no* changes for it: it already writes events to the DB and
reads prompts/bios from disk; the dashboard only saves files (mtime hot-reload) and can
trigger `.ai reload` if we later expose a console/HTTP trigger.

Not embedded in worldserver: TC's built-in HTTP framework is inbound-only for the game
server and mixing a web app into the game loop/port is off-pattern and a stability risk.

### 5.1 Screens / features

| Screen | Features |
|---|---|
| **Events** | live table (auto-refresh ~2s), filter by type / entity / time range, row detail (payload + related JSON); **red badge** on every row with `ai_processed = 0` ("not included in AI processing"); clear-for-testing button (gated) |
| **NPCs** | list from `ai_character` (entry, name, enabled, model/lang/temp overrides, last_seen); **add** (pick entry → auto-create bio + row), **edit bio** (markdown editor + preview), enable/disable, per-NPC overrides; per-entry view + a per-**spawn** event stream drill-down (from `ai_event.actor_uid`) |
| **Prompts** | tree of `AI/prompts/**`; edit any `.md` (or `slots.txt`); placeholder validation (flag unknown `{{x}}`); **live assembled-prompt preview** for a chosen type + sample entities |
| **Config** | read-only view of relevant `AISystem.*` + `AIDatabaseInfo` sanity (is the DB reachable? is the key set?) |

### 5.2 Notes

- **Hot reload:** dashboard saves edits to disk → worldserver picks them up on next request
  (mtime) or after `.ai reload`.
- **Security:** bind localhost only, no auth (local dev). Future: token auth + read-only
  mode.
- **No conflicting writers:** worldserver is the only *writer* of `ai_event`; dashboard
  only reads it. Both may write `ai_character` + bio files (worldserver auto-creates,
  dashboard edits) — last-write-wins is acceptable.

---

## 6. `ai reload` command

`cs_ai.cpp` adds `.ai reload`: flush the prompt template cache, re-index `characters/`,
log `AISystem` "prompt cache reloaded (N templates, M bios)". RBAC: reuse an existing perm
(e.g. `RBAC_PERM_COMMAND_NPC_SAY`) — no new perms.

---

## 7. Acceptance / smoke test

1. Fresh boot with no `ai/` folder → worldserver seeds default templates; runs unchanged.
2. With a player near an NPC and `AISystem.Enabled=1`: `.npc say "hello"` → second NPC
   replies; `characters/<entry> - <name>.md` exists for both; edit one bio → next reply
   reflects it (hot reload, no restart).
3. **Proximity gating:** with NO player nearby, an NPC says/kills/aggros → **zero** events
   logged, no AI activity. A player walks into `ActivationRadius` → events begin logging;
   walks out → logging stops. Applies to death/combat too (e.g. kill an NPC while the
   player is near → `death` row; far away → no row).
4. **Spawn identity:** two different spawns of the same entry produce two distinct event
   streams (`npc_spawn:<g1>` vs `npc_spawn:<g2>`), but share one bio file.
5. Language: default → replies in English. Set `AISystem.Language = pt` → Portuguese.
   Per-NPC override in `ai_character` wins for that NPC.
6. `ai_npc.ai_event` accumulates `player_say`/`npc_say` rows while chatting near a player;
   `{{events}}` in the assembled prompt shows the recent lines involving the involved
   spawns; injected rows are `ai_processed=1`.
7. With `AISystem.Enabled=0` (AI replies off), events **still** log while a player is near,
   and the dashboard shows them with the **red "not included in AI processing"** marker.
8. Edit a template file → next in-game reply uses it; `.ai reload` works.
9. Dashboard: lists events/NPCs/prompts; bio edit → new `{{bio}}`; kill the DB / stop MySQL
   → worldserver keeps running, events dropped with an `AISystem` error, no freeze.
10. `ai off` session → no AI replies for that session; events near a player still logged.

---

## 8. Files touched

- **New (module):** `src/server/game/AI/LivingNPC/AIEvent.{h,cpp}` (event structs, type
  table, `Log()` → `AIDatabase` prepared statement), `AIPrompt` rewrite → file-driven
  template loader + placeholder resolver + cache (§2.5), bio lookup/auto-create (§2.6),
  event-injection query + `ai_processed` marking (§3.7).
- **New (DB):** `AIDatabase` pool (following the `Login/Character/WorldDatabase` pattern),
  `sql/ai_npc/001_schema.sql`.
- **Edit:** `AIConfig.{h,cpp}` (new keys: Language, PromptRoot, EventLog, ActivationRadius,
  MaxInjectedEvents, EventTrimChars, SummaryChars), `AIManager.{h,cpp}` (language
  resolution, proximity gate, event emission in `OnPlayerSpeak`/`OnNpcSpeak`/drain, bio
  auto-create call, request-type detection → prompt type), `World.cpp` (`AIDatabase`
  start/stop + `World::Update` drain emits `npc_say` for LLM replies),
  `cs_ai.cpp` (`.ai reload`), `ChatHandler.cpp` / `cs_npc.cpp` (already hooked — confirm
  event emit), `worldserver.conf.dist` (new keys + `[connections] AIDatabaseInfo`).
- **New (dashboard):** `tools/ai_dashboard/` (FastAPI app + static SPA), read-only DB
  access to `ai_npc`, read/write of `AI/`.

---

## 9. Decisions (confirmed 2026-10-04)

1. **Storage:** ✅ `ai_npc` schema on the same MySQL server, new `AIDatabase` pool (§3.5).
2. **WebUI stack:** ✅ FastAPI sidecar (most efficient / easiest to implement) (§5).
3. **Identity:** ✅ bios per **entry**; events & summaries per **spawn GUID** (§2.6, §3.2).
4. **Gating:** ✅ engine + events active **only** in a player's immediate periphery
   (`ActivationRadius`), for **every** event type and every trigger; when a player is near,
   events fire for **any** NPC or player in the periphery (§3.6).
5. **Dashboard marker:** ✅ events logged independently of `AISystem.Enabled` (gated by
   proximity only); rows not fed into an LLM prompt show a **red "not included in AI
   processing"** badge via `ai_processed` (§3.7, §5.1).

---

## 10. Implementation status (2026-10-04)

**Implemented on `feature/ai-npc` and smoke-tested.** `worldserver` rebuilt (0 errors);
a boot of the new binary seeded the `ai/prompts/` default tree, opened the `ai_npc`
`AIDatabase` pool (2 live connections verified), and reached "World initialized" without
crash. The FastAPI dashboard runs (verified: 23 prompt files readable, config/status/
preview endpoints live).

- **DB:** `src/server/database/Database/Implementation/AIDatabase.{h,cpp}` (new connection
  class), `DatabaseEnv.{h,cpp}` (register `AIDatabase` pool),
  `DatabaseWorkerPool.cpp` (explicit instantiation for `AIDatabaseConnection`),
  `sql/ai_npc/001_schema.sql`.
- **Events:** `AI/LivingNPC/AIEvent.{h,cpp}` — async event log + spawn-keyed UIDs +
  player-proximity gate + `{{events}}` injection query with `ai_processed` marking.
- **Prompts:** `AI/LivingNPC/AIPrompt.{h,cpp}` — file-driven slot engine (cache + mtime),
  placeholder resolver, per-entry bio auto-create + index; `AIContext::GetSummary`.
- **Config:** `AIConfig.{h,cpp}` (Language, PromptRoot, EventLog, ActivationRadius,
  MaxInjectedEvents, EventTrimChars, SummaryChars) + `worldserver.conf.dist` +
  local `worldserver.conf` (untracked) with `AIDatabaseInfo`.
- **Manager/hooks:** `AIManager` (language resolution, proximity gate, event emission,
  request-type prompts); `Unit::Kill`/`Unit::CombatStop`/`CombatManager::SetInCombatWith`
  emit death/combat events; ambient narrator now respects `ActivationRadius` and actually
  ticks (fixed a latent `UpdateAmbient(0)` bug from Spec 001).
- **Command:** `.ai reload` in `cs_ai.cpp`.
- **Dashboard:** `tools/ai_dashboard/` (FastAPI + SPA, `start.bat` launcher).

Known caveats (unchanged from Spec 001): `{{memories}}` empty until Spec 003; per-spawn
identity for memory deferred; virtual `player_entered_range` type exists but no hook is
wired yet.
