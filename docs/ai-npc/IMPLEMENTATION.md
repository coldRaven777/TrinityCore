# LivingNPC (Alive NPCs) — Implementation Reference (as-built)

> **Scope:** This document describes the *actual* code in `src/server/game/AI/LivingNPC/`
> as it exists on branch `feature/ai-npc` (compiled into `build/bin/RelWithDebInfo/worldserver.exe`,
> built 2026-10-04). It is the machine-readable source-of-truth for the AI feature so the
> agent stays aligned across sessions. Where the code differs from the design docs
> (`PROJECT.md`, `SPEC-001.md`), the **deltas are called out in §9**.
>
> Companion design docs (still useful for intent/roadmap): `PROJECT.md` (architecture, risk,
> roadmap) and `SPEC-001.md` (spec + acceptance). This file wins on any conflict.

---

## 0. Quick orientation

| Item | Value |
|------|-------|
| Module name | `LivingNPC` (a.k.a. "Alive NPCs") |
| Backend | OpenRouter `https://openrouter.ai/api/v1/chat/completions` |
| Code root | `src/server/game/AI/LivingNPC/` |
| Branch | `feature/ai-npc` |
| Build target | `worldserver` (game lib) + `cs_ai` (scripts) |
| Logger | `AISystem` (conf: `Logger.AISystem=4,Console Server`) |
| Outbound HTTP | Custom `boost::beast` + OpenSSL client (TC ships **no** outbound HTTP client) |
| Config keys | `AISystem.*` in `worldserver.conf` (NOT in TC's `WorldBool/Int/Float` enums) |

**One sentence:** when a player or NPC *says* something, `AIManager` picks responder(s),
fires an async OpenRouter chat completion on a private worker thread, and `World::Update`
later makes the Creature speak the result — the game loop is never blocked.

---

## 1. Threading model (CRITICAL — read before touching anything)

`worldserver` runs the game loop on a **single main thread**. OpenRouter round-trips take
~0.3–3 s, so **all** LLM I/O is isolated:

```
main game thread                          AIService worker thread
─────────────────                        ────────────────────────
ChatHandler::OnPlayerSpeak / cs_npc        AIService::Impl::Run()  (ioc.run())
  -> AIManager::OnPlayerSpeak                posts AIRequestOp to its own io_context
     -> AIService::RequestChatCompletion    resolves+connects+handshakes+POST+reads
        (post to ioc, returns instantly)    parses content, invokes callback
  ...game keeps running...                     callback pushes AIResult into m_results (mutex)
World::Update() each tick
  -> sAIManager->Update(diff)
     -> drains m_results (mutex swap)
     -> Creature::Say(r.text)   <-- game objects touched ONLY here
```

Rules the implementation enforces:
- **Only `AIService` touches the socket** (its own `boost::asio::io_context` + `std::thread`).
- **Only `World::Update` touches `Creature`/`Map`** — all `Creature::Say` happens on the main thread.
- The shared state is exactly three mutex-guarded structures in `AIManager`:
  `m_results` (result queue), `m_npcCooldown` (per-NPC reply cooldown), `m_sessionEnabled`
  (per-account on/off). `AIContext::m_history` has its own mutex.
- The async callback runs on the worker thread and **must not** touch game objects — it only
  records history and `EnqueueResult(...)`.

`AIService::Impl` uses `boost::asio::executor_work_guard` (since Boost 1.70 the `io_context::work`
replacement) to keep `ioc.run()` alive between requests; `Stop()` releases the guard, stops the
ioc, and joins the thread.

---

## 2. Files and responsibilities

All under `src/server/game/AI/LivingNPC/` unless noted.

| File | Responsibility |
|------|----------------|
| `AIConfig.{h,cpp}` | Reads `AISystem.*` keys from `worldserver.conf` into cached fields. Singleton `sAIConfig`. |
| `AIService.{h,cpp}` | Async + sync OpenRouter client. Builds JSON, does HTTPS POST, parses `choices[0].message.content`. Owns the worker thread. |
| `AIDecision.{h,cpp}` | "Who should reply?" — small **sync** OpenRouter call; rule-based fallback (`RuleFallback`). |
| `AIContext.{h,cpp}` | Per-conversation rolling history (keyed by speaker spawn GUID) + token-budget % split logic. |
| `AIPrompt.{h,cpp}` | Builds the in-character system/persona prompt from name/type/zone/locale. |
| `AIManager.{h,cpp}` | Orchestrator: target selection, enqueue, result drain, ambient narrator timer, session toggles. Singleton `sAIManager`. |
| `src/server/scripts/Commands/cs_ai.cpp` | Chat commands `.aisai`, `.ai on`, `.ai off`. |
| `src/server/game/Handlers/ChatHandler.cpp` | `CHAT_MSG_SAY` hook → `sAIManager->OnPlayerSpeak`. |
| `src/server/scripts/Commands/cs_npc.cpp` | `.npc say` hook → `sAIManager->OnNpcSpeak` (ambient NPC→NPC). |
| `src/server/game/World/World.cpp` | `sAIManager->Initialize()` at startup; `sAIManager->Update(diff)` inside `World::Update`. |

---

## 3. `AIConfig` — configuration

Loaded once by `AIManager::Initialize()` via `sAIConfig->Load()`. All keys are read with
`GetBool/Int/Float/StringDefault` straight from `sConfigMgr` — **they are deliberately not part
of TC's typed config enums**. Defaults in code (`AIConfig.cpp`) vs. the `.conf.dist` block:
they must match or the doc is lying — see §9 for current state.

Exposed accessors (the rest of the code reads config ONLY through these):
- `IsEnabled()`, `UseDecisionModel()`, `AmbientNeedsPlayer()`
- `MaxTokens()`, `EarshotRadius()`, `RequestTimeoutSec()`, `CooldownSec()`, `NpcChatCooldownSec()`
- `Temperature()`, `ContextSystemRatio()`, `ContextHistoryRatio()`, `InterjectChance()`
- `OpenRouterApiKey()`, `ChatModel()`, `DecisionModel()`, `HasValidKey()` (`!m_apiKey.empty()`)

`Load()` emits `AISystem` error if `Enabled=1` but the key is empty, and the system then runs
with LLM calls disabled (idle, no crash).

---

## 4. `AIService` — the HTTP client

### 4.1 Public API
```cpp
class AIService {
  void Start();          // spin worker thread (idempotent)
  void Stop();           // release work guard, stop ioc, join
  void RequestChatCompletion(apiKey, model, messages, temperature, maxTokens,
                             timeoutSec, AIChatCallback);   // ASYNC (worker thread)
  bool RequestChatCompletionSync(apiKey, model, messages, temperature, maxTokens,
                                 timeoutSec, std::string& outReply);  // SYNC (decision only)
  static AIService* Get();
  static bool ExtractContent(body, out);   // parse choices[0].message.content
  static std::string BuildJson(model, messages, temperature, maxTokens);
};
```

### 4.2 `AIMessage` / callback
```cpp
struct AIMessage { std::string role;   // "system" | "user" | "assistant"
                   std::string content; };
using AIChatCallback = std::function<void(bool success, std::string reply, std::string error)>;
```

### 4.3 Request details
- **Host:** `openrouter.ai`, port `443`, TLS (SNI set via `SSL_set_tlsext_host_name`, required
  for the Cloudflare-fronted host).
- **Verify mode:** `bs::verify_none` — cert validation is **disabled** (Windows OpenSSL ships
  no default CA bundle). Dev convenience; note for any production hardening.
- **Headers:** `Authorization: Bearer <key>`, `Content-Type: application/json`,
  `User-Agent: TrinityCore-LivingNPC`, `HTTP-Referer: https://trinitycore.org`,
  `X-Title: TrinityCore LivingNPC`.
- **Target/path:** `POST /api/v1/chat/completions`.
- **Body** (`BuildJson`): `{"model":...,"temperature":...,"max_tokens":...,"stream":false,"messages":[...]}`.
  JSON is hand-built with a `JsonEscape` helper (escapes `" \ ` and control chars); no JSON lib.
- **Parsing** (`ExtractContent` + `FindJsonString`): a hand-rolled JSON string extractor that
  finds `"choices"`, then `"content"` inside, handling `\uXXXX` and escapes. On API error it
  tries to surface `"message"` as the error text.

### 4.4 Async vs sync
- **Async** (`RequestChatCompletion`): posts an `AIRequestOp` (self-referencing `shared_ptr`) to
  the worker `io_context`; the callback runs on the worker thread and must only push to
  `AIManager::m_results` + history.
- **Sync** (`RequestChatCompletionSync`): spins up a *private* `io_context`, drives it with
  `poll_one()` inside a `steady_timer`-guarded loop (timeout → returns `false`), used only for
  the fast/cheap decision call in `AIDecision::Decide`. It runs on whatever thread calls it
  (main), so it is small (maxTokens 16, temperature 0.2) to stay short.

---

## 5. `AIManager` — orchestration

### 5.1 Lifecycle
- `Initialize()` (called from `World.cpp` at startup): `sAIConfig->Load()`; if enabled,
  `AIService::Get()->Start()` and logs. Idempotent-ish; sets `m_initialized`.
- `Update(uint32 diff)` (called from `World::Update` every tick): drains `m_results` and calls
  `Creature::Say` on the correct map/instance; then `UpdateAmbient(diff)`.

### 5.2 Hooks
- `OnPlayerSpeak(Player*, text)` — from `ChatHandler` `CHAT_MSG_SAY`. Gates on
  `IsEnabled() && SessionAllowed(player) && !text.empty()`.
  1. Collect NPCs within `EarshotRadius` of the player (`CollectNearby` walks
     `Map::GetCreatureBySpawnIdStore()`). Bail if none.
  2. **Responder selection:**
     a. Selected unit that is a Creature and within radius → it responds.
     b. Else the Creature most in front of the player (facing dot > `0.3`) within radius.
     c. Else, if `UseDecisionModel && nearby.size()>1 && HasValidKey()` → `AIDecision::Decide(...)`
        picks among them; rule fallback = `nearby.front()`.
  3. `RequestReply(responder, player, text, false)`.
  4. `MaybeInterject(responder, player, text)` — with `InterjectChance`, a *different* ear-shot
     NPC fires a gated second call (role = interjection).
- `OnNpcSpeak(Creature* speaker, text)` — from `.npc say` (`cs_npc.cpp`).
  1. Collect other Creatures within radius.
  2. If exactly one other → it responds directly. Else decision model (if enabled+key) or
     `others.front()`. `userLine` is prefixed `"[<speaker name>] said: <text>"`.

### 5.3 `RequestReply` flow
- Per-NPC cooldown gate: `m_npcCooldown[spawnId]` set to `gameTime + CooldownSec()`; early-return
  if still cooling (prevents reply spam).
- Builds system prompt via `AIPrompt::BuildSystem(name, "creature", zone, locale)` where
  `locale = LocaleFor(player)` → currently **hardcoded `"ptBR"`** (see §9).
- `convKey = to_string(speaker spawnId)`. Builds messages via `AIContext::BuildMessages`.
- Fires `AIService::RequestChatCompletion`; the callback:
  - trims/normalizes reply (strips quotes/whitespace, caps at 2000 chars),
  - records both the user line and the reply into `AIContext` history,
  - builds `AIResult{mapId, instanceId, spawnId, text, isInterject}` and `EnqueueResult`.

### 5.4 `Update` drain
Swaps the whole `m_results` queue under lock, then for each result finds the Creature on its map
via `sMapMgr->FindMap(mapId, instanceId)` → `GetCreatureBySpawnId(spawnId)` and calls
`c->Say(text, LANG_UNIVERSAL)` (only if alive). This is the **only** place game objects are touched.

### 5.5 Ambient narrator (`UpdateAmbient`)
- Gated by `NpcChatCooldownSec` (0 disables). Respects `AmbientNeedsPlayer` (bails if no active
  session).
- Every `NpcChatCooldownSec` seconds picks a **random alive creature** on the first session
  player's map and asks the chat model for a spontaneous in-character line ("thinking aloud").
  Goes through the same `AIService` → `Update` → `Say` path (history appended as assistant).

### 5.6 Session toggles
- `SetSessionEnabled(accountId, on)` / `IsSessionEnabled(accountId)`: per-account map
  (`m_sessionEnabled`); `accountId == 0` flips the global default (`m_globalEnabled`).
- `SessionAllowed(player)`: console/non-player → global default; else the account's setting.

---

## 6. `AIDecision` — who replies

- `Decide(apiKey, model, candidates[{guid,name}], lastLine, timeoutSec)`:
  - 0 candidates → `""`; 1 candidate → that GUID directly (no call).
  - Builds a narrator prompt ("Reply with ONLY the GUID number") + the candidate list, calls
    `AIService::RequestChatCompletionSync` (maxTokens 16, temp 0.2).
  - Extracts the first run of digits, validates it matches a candidate GUID; on any miss →
    `RuleFallback`.
- `RuleFallback`: lexicographically first candidate **name** (stable, deterministic).
- Decision model id from config = `AISystem.DecisionModel`.

---

## 7. `AIContext` — history & % split

- Per-conversation history keyed by `convKey` (speaker spawn GUID string) in
  `std::map<std::string, std::vector<AIMessage>> m_history`, mutex-guarded.
- `AddTurn(key, role, content)`: appends; caps stored history at **40 turns** (safety; real trim
  is by token budget in `BuildMessages`).
- `BuildMessages(key, systemPrompt, currentUserLine, historyRatio, maxTokens)`:
  - starts with the system message;
  - `historyBudget = maxTokens * historyRatio`;
  - token estimate = `size/4 + 1`;
  - trims **oldest-first** until the (system + history) budget fits (always keeps at least the
    last history entry);
  - appends `currentUserLine` as a `user` message if non-empty.
- `EstimateTokens` is a crude `size/4+1` heuristic.

---

## 8. `AIPrompt` — persona

`BuildSystem(name, type, zone, locale, extra="")`:
```
You are <name>, a <type|creature> in the world of Warcraft[, currently in <zone>].
Stay fully in character ... Never mention that you are an AI ... Respond naturally
and concisely ... Keep replies short (one or two sentences) ...
Always reply in the language '<locale|enUS>'. [extra]
```
`type` is passed as the literal `"creature"` from `AIManager` (no deeper creature-type
subtype is wired in yet). `locale` is currently always `"ptBR"` (see §9).

---

## 9. As-built deltas vs design docs (read carefully)

The design docs (`PROJECT.md`, `SPEC-001.md`) were written pre-implementation. The code on
`feature/ai-npc` diverged in these specific ways — **the code is authoritative**:

1. **Config defaults differ.** `AIConfig.cpp` code defaults vs `worldserver.conf.dist` block:
   - `MaxTokens`: code `1024` / conf.dist `220`. (Conf value 220 wins at runtime when present;
     the doc default text is stale.)
   - `ContextSystemRatio`: code `0.35` / conf.dist `0.30`.
   - `ContextHistoryRatio`: code `0.55` / conf.dist `0.60`.
   - `DecisionModel` default: code `anthropic/claude-3-haiku` / conf.dist `openai/gpt-4o-mini`.
   - **Action:** fix `worldserver.conf.dist` to match `AIConfig.cpp` (or vice-versa). They must agree.
2. **Decision model default model id** is `anthropic/claude-3-haiku` in code, not `gpt-4o-mini`.
3. **Locale is hardcoded** `LocaleFor()` returns `"ptBR"` unconditionally (ignores any
   session/player language). Design wanted locale inference — not done yet.
4. **Cert verification is OFF** (`bs::verify_none`) — a deliberate Windows-dev workaround, not
   in the design docs. Note for production.
5. **No JSON library** — `BuildJson`/`ExtractContent`/`FindJsonString` are hand-rolled string
   parsers, not nlohmann/rapidjson as the design mused. Fragile to OpenRouter schema changes;
   keep an eye on it.
6. **Reply cap:** 2000 chars (code), not mentioned in design.
7. **Decision model uses the SYNC client** (`RequestChatCompletionSync`) — design implied it
   would be async; it's sync but tiny, so acceptable.
8. `AISystem.ModelChat` in the design conf table is actually named **`AISystem.ChatModel`** in
   `conf.dist` and code — naming fixed, doc table was slightly off.

None of these break the build or runtime; items 1 (default mismatch) is the one worth reconciling.

---

## 10. Commands (runtime)

| Command | Source | Behaviour |
|---------|--------|-----------|
| `.aisai "<text>"` | `cs_ai.cpp` | Selected NPC says `<text>` instantly via `Creature::Say` — **no LLM**. Needs a selected creature (`LANG_SELECT_CREATURE` guard). RBAC `COMMAND_NPC_SAY`. |
| `.ai on` | `cs_ai.cpp` | `SetSessionEnabled(account, true)` for the calling account. RBAC `COMMAND_NPC_INFO`. |
| `.ai off` | `cs_ai.cpp` | `SetSessionEnabled(account, false)`. |
| (implicit) player `/say` near NPC | `ChatHandler` | `OnPlayerSpeak` → responder + possible interjection. |
| (implicit) `.npc say "<text>"` near NPC | `cs_npc.cpp` | `OnNpcSpeak` → ambient NPC→NPC responder. |

---

## 11. Enabling the feature (operational)

1. In **local untracked** `worldserver.conf`:
   - `AISystem.Enabled = 1`
   - `AISystem.OpenRouterApiKey = "sk-or-..."`  ← never commit; `.conf.dist` keeps this `""`.
   - optionally tune `AISystem.ChatModel` / `DecisionModel` / `MaxTokens` / `EarshotRadius` / etc.
2. The key lives **only** in your local `worldserver.conf` (git-untracked). `.conf.dist` documents
   the key names with empty/placeholder values. See `PROJECT.md` §2 for the fork-safety rule.
3. Restart `worldserver`. Expect `AISystem` log lines:
   `AIManager initialized (feature enabled).` and `AIService started (async HTTP worker thread).`
4. Smoke test (from `SPEC-001` §7, still valid): select NPC, `.npc say "hello"` near a 2nd NPC →
   2nd NPC replies via LLM (watch `Server.log` `AISystem`); `.aisai` is instant; player `/say`
   near NPC gets an answer; `ai off` silences the session; bad key/network → server stays up,
   turns dropped with an `AISystem` error (no freeze).

---

## 12. Known limitations / open work (as-built)

- **No long-term memory** — only rolling in-context history per `convKey` (speaker spawn GUID).
  Conversations are per-creature and do not survive restart.
- **Locale hardcoded ptBR** — multi-locale not wired (§9.3).
- **No per-NPC persona/knowledge injection** — `type` is always `"creature"`, `extra` always
  empty. Roadmap items in `PROJECT.md` §10 (Spec 2 memory, Spec 3 knowledge, Spec 4 personas)
  remain unimplemented.
- **Hand-rolled JSON** (§9.5) — brittle; if OpenRouter changes response shape it breaks silently
  (will log `unexpected response` / `API error`).
- **Cert verification off** (§9.4) — dev convenience, not production-safe.
- **Decision/rule fallback** is lexicographic name sort (deterministic but not semantic).

---

## 13. Build / rebuild notes

- The feature is compiled into `worldserver.exe` (last build 2026-10-04, present at
  `build/bin/RelWithDebInfo/worldserver.exe` + `.pdb`).
- Building the whole game lib is slow; the helper scripts at repo root (`build_worldserver.bat`
  / `build_ai.bat`) drive the CMake/VS build. After editing any `LivingNPC` file, rebuild
  `worldserver` and restart.
- Sources are auto-globbed by TC's `CollectAndAddSourceFiles` for `src/server/game/...`, but the
  `scripts/Commands/cs_ai.cpp` registration is in `cs_script_loader.cpp` (verify it is still
  registered after merges — see git status showing `cs_script_loader.cpp` modified).
