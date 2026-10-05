# Spec 001 — Alive NPCs: foundation, `aisai`, and player-triggered response

**Depends on:** PROJECT.md §2–§8
**Scope (this spec only):**
1. Config plumbing (`AISystem.*`) + enable flag.
2. Outbound async HTTP client to OpenRouter (`AIService`).
3. `aisai "text"` command (force selected NPC to say text).
4. Player SAY near NPC(s) → decision → selected/front NPC answers; earshot NPCs may interject.
5. Ambient NPC→NPC: when an NPC says something near other NPCs, decision model picks a responder.
6. Context % split (system/history) with trimming; no long-term memory yet.

**Out of scope (later specs):** memory, knowledge injection, personas, timed group chat.

---

## 1. Config (`worldserver.conf.dist` additions, key empty by default)

Add the block from PROJECT.md §7 to `src/server/worldserver.conf.dist` so the server parses
the keys. Actual key + enable=1 live in the **local untracked** `worldserver.conf`.

Implementation: new `AIConfig` loaded in `World::LoadConfig` (or `AISystem` lazy-init on
first use). Keys parsed via `sConfigMgr->GetBool/GetString/GetFloat/GetInt`.

## 2. `AIService` — async outbound client

- New files `src/server/game/AI/LivingNPC/AIService.{h,cpp}`.
- Holds `boost::asio::io_context` + a `std::thread` running `io_context.run()` (started in
  `World::StartSystem` / lazy init, stopped on shutdown).
- `AIService::ChatCompletionAsync(model, messages, temperature, maxTokens, timeoutSec,
  callback)` posts an async HTTPS POST (boost::beast) to OpenRouter, parses
  `choices[0].message.content`, and invokes `callback(std::string reply)` — **on the worker
  thread**. The callback must NOT touch game objects; it only pushes into the return queue.
- Return queue: `std::mutex` + `std::queue<AIResult>` where `AIResult { CreatureGuid,
  std::string text, bool interject }`. `World::Update` pops and dispatches `Creature::Say`
  on the game thread.
- On error/timeout: log `AISystem` warning, drop the turn (no crash, no freeze).

## 3. `aisai "text"` command — `src/server/scripts/Commands/cs_ai.cpp`

- New `CommandScript` `ai_commandscript` registering:
  - `aisai <Tail text>` → `HandleAisaiCommand`: get `handler->getSelectedCreature()`;
    if none, `LANG_SELECT_CREATURE` + error (same guard as `HandleNpcSayCommand`).
    Then `creature->Say(text, LANG_UNIVERSAL)` + emote (reuse exact pattern). **No LLM.**
  - `ai <on|off>` → toggles `AIManager::SetSessionEnabled(player, bool)` (session-scoped;
    global fallback via config if no player session, e.g. console).

## 3b. Ambient narrator pass (added by decision)

- A per-map timer in `AIManager` runs every `AISystem.NpcChatCooldownSec` (default 50) and,
  **only if at least one player is on the map** (`AISystem.AmbientNeedsPlayer`), calls the
  decision/"narrator" model to select one NPC to speak spontaneously (an opening line or a
  reply to the last ambient utterance). The selected NPC goes through the normal
  `AIService` → `World::Update` → `Creature::Say` path. This is the cadence between narrator
  selections, distinct from the per-NPC `CooldownSec` that gates reply spam.

## 4. Player SAY hook — `Handlers/ChatHandler.cpp`

In `HandleChatMessage` (the `CHAT_MSG_SAY` case, after validation), call
`AIManager::OnPlayerSpeak(Player* player, std::string const& text)` **asynchronously**
(enqueue; do not block). `AIManager`:
- gathers NPCs within `EarshotRadius` of the player on the same map;
- target selection:
  - if `player->GetSelection()` is a Creature in earshot → that NPC is the speaker;
  - else if exactly one Creature is directly in front (dot product of facing vs. player→NPC
    vector > threshold, within radius) → that NPC;
  - else if `UseDecisionModel` → `AIDecision::PickResponder(...)`; else rule-based (closest);
- builds prompt (persona + trimmed history) and calls `AIService::ChatCompletionAsync`;
- on result: `Creature::Say(reply, LANG_UNIVERSAL)`; then with `InterjectChance`, pick
  another earshot NPC and fire a second (gated) call for an interjection.

## 5. Ambient NPC→NPC — `cs_npc.cpp` `.npc say` + `CreatureTextMgr`

`HandleNpcSayCommand` (and any NPC text emit path we route through `AIManager::OnNpcSpeak`)
calls `AIManager::OnNpcSpeak(Creature* speaker, std::string const& text)`:
- find other Creatures within `EarshotRadius`;
- if only one other → it responds directly (no decision call);
- else `AIDecision::PickResponder` (or rule-based) chooses the responder, which then goes
  through the same `AIService` → `World::Update` → `Creature::Say` path.

## 6. Context % split — `AIContext`

- `AIContext` per (speaker GUID + audience key) ring buffer of `{role, text}` up to a token
  budget = `maxTokens * ContextHistoryRatio` (token estimate: `text.size()/4`).
- System slice = `maxTokens * ContextSystemRatio`, remainder = headroom.
- Trim oldest history entries when over budget.
- System prompt default: short persona ("You are <name>, a <type> in <zone>. Speak in
  <locale>. Be concise, in character, never break character or mention you are an AI.").
  Locale inferred from player or `AISystem.ResponseLocale` (default ptBR to match client).

## 7. Acceptance / smoke test

1. Build `worldserver` (toolchain skill). Start with `AISystem.Enabled = 0` → server runs,
   no behaviour change.
2. Set `Enabled=1`, put key in local conf, restart. Select an NPC, `.npc say "Hello
   everyone"` near a second NPC → second NPC replies via LLM (visible in game + `Server.log`
   `AISystem` lines).
3. `aisai "Welcome traveler"` → selected NPC says it (instant, no LLM).
4. Player types `/say` near an NPC → that NPC answers; a nearby second NPC sometimes
   interjects.
5. Kill network / bad key → server keeps running, turns are dropped with a warning (no freeze).
6. `ai off` → no AI replies for that session.

## 8. Files touched

- **New:** `src/server/game/AI/LivingNPC/{AIService,AIManager,AIConfig,AIDecision,AIContext,AIPrompt}.{h,cpp}`,
  `src/server/scripts/Commands/cs_ai.cpp`.
- **Edit:** `src/server/game/Handlers/ChatHandler.cpp` (player SAY hook),
  `src/server/scripts/Commands/cs_npc.cpp` (`.npc say` ambient hook),
  `src/server/game/World/World.cpp` (`World::Update` queue drain + service lifecycle),
  `src/server/worldserver.conf.dist` (config keys),
  `CMakeLists.txt` (add new sources to `game`/`scripts` targets).

## 10. Open questions to confirm before coding

- ~~Default model id (we'll default `openai/gpt-4o-mini`; you can change in conf). Do you have
  an OpenRouter key ready, or should we stub the client behind a "mock mode" that echoes so
  you can test offline first?~~ **DECIDED: real OpenRouter client; key provided (kept in local
  untracked worldserver.conf only).**
- ~~Confirm `LivingNPC` module name (or rename to `AISpeech`/`LLMNPC`).~~ **DECIDED: `LivingNPC`.**
- ~~Should ambient NPC→NPC chatter be on by default when enabled, or only when a player is
  present?~~ **DECIDED: only when a player is present** (`AISystem.AmbientNeedsPlayer = 1`),
  with a configurable `AISystem.NpcChatCooldownSec = 50` between narrator selections.

## 11. Implementation status (2026-10-04)

Implemented on branch `feature/ai-npc` (config enums + load table, worldserver.conf.dist keys,
the `LivingNPC` module, hooks, command `cs_ai.cpp`, World::Update drain + Initialize). Build
pending verification.

Files:
- Config: `World.h` (enum entries), `World.cpp` (load table), `worldserver.conf.dist` (keys).
- Module: `src/server/game/AI/LivingNPC/{AIConfig, AIService, AIContext, AIPrompt, AIDecision,
  AIManager}.{h,cpp}`.
- Commands: `src/server/scripts/Commands/cs_ai.cpp` (+ `cs_script_loader.cpp` registration).
- Hooks: `Handlers/ChatHandler.cpp` (player SAY), `scripts/Commands/cs_npc.cpp` (`.npc say`),
  `World.cpp` (Initialize + Update drain).

