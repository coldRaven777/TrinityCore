# Project: Alive NPCs — LLM-driven NPC conversation for TrinityCore

> Private fork: `coldRaven777/TrinityCore` (branch `master`, 12.x / retail 12.1.0.69933)
> Upstream: `TrinityCore/TrinityCore` (kept in `upstream` remote)
> Author: you + Hermes agent | Status: design phase, Spec 1 pending

---

## 1. Goal

Make every Creature (NPC / MOB) capable of speaking through an LLM naturally — to the
player and to each other — using **OpenRouter** as the default backend. Behaviour is
driven by chat commands and by proximity ("earshot"). No long-term memory in this phase;
only the rolling in-context chat history, split so a fraction of the context is reserved
for prompt engineering / future memory + knowledge injection.

---

## 2. Repository & branch strategy (keep fork private + updated)

Current state (verified):
- `origin` = `coldRaven777/TrinityCore` (your **private** fork) — push here only.
- `upstream` = `TrinityCore/TrinityCore` — pull from here to stay current.
- You have 3 uncommitted local tweaks: `DB2Stores.cpp` (DB2 required-item demotion), and
  two `sql/old/12.x/...` dropped-column fixes. **Commit these to your fork first** so they
  are preserved and don't collide with future `git merge upstream/master`.

Workflow we will follow:
1. Work on a feature branch `feature/ai-npc` branched from `master`.
2. Keep `master` as the stable, deployable branch of your fork.
3. Sync with upstream regularly without clobbering your work:
   ```
   git fetch upstream
   git checkout master
   git merge upstream/master        # resolve conflicts on your branch, then
   git checkout feature/ai-npc
   git merge master                 # bring upstream+our updates into feature
   ```
4. Before merging feature → master: build, run, smoke-test on Windows (see toolchain skill).
5. **Never commit the OpenRouter API key.** `worldserver.conf` is already untracked locally
   (only `.conf.dist` ships in the repo). Put `AISystem.OpenRouterApiKey` in your local
   `worldserver.conf` (untracked) and document the key in `worldserver.conf.dist` as empty.
   Add `AISystem.*` config keys to `worldserver.conf.dist` so the server starts without the
   feature enabled by default.

> Design docs (`docs/ai-npc/*`) live in the repo but the fork is private, so that is fine.
> If you'd rather keep them out of git, add `docs/ai-npc/` to `.gitignore`.

---

## 3. Architecture overview

```
                         ┌─────────────────────────────────────────────┐
                         │  worldserver  (main game thread, single)    │
                         │                                             │
   player SAY ──────────►│  ChatHandler::HandleChatMessage             │
   .npc say / aisai ────►│  cs_npc HandleNpcSayCommand / aisai cmd     │
                         │        │                                    │
                         │        ▼                                    │
                         │  AIManager (per-map or global singleton)   │
                         │   - decide target NPC(s) by prox + rules    │
                         │   - build prompt (system + history split)  │
                         │   - enqueue async LLM request              │
                         └───────────────┬───────────────────────────┘
                                         │  (non-blocking post)
                                         ▼
                         ┌─────────────────────────────────────────────┐
                         │  AIService  (own boost::asio io_context +    │
                         │   worker thread) — does NOT touch game loop │
                         │   - outbound HTTPS POST to OpenRouter        │
                         │   - parse choices[0].message.content        │
                         │   - on done: push result into thread-safe   │
                         │     return queue                            │
                         └───────────────┬───────────────────────────┘
                                         │  result queued
                                         ▼
                         ┌─────────────────────────────────────────────┐
                         │  World::Update  drains the return queue each │
                         │  tick and calls Creature::Say on the map's   │
                         │  thread (correct thread → safe). NPCs in     │
                         │  earshot "hear" it and may interject.        │
                         └─────────────────────────────────────────────┘
```

### Why async is mandatory
`worldserver` runs the game loop on one thread. An OpenRouter round-trip is 0.3–3 s over
the network. A **blocking** call would freeze every player on the map. So all LLM I/O lives
in `AIService` with its own `boost::asio::io_context` + worker thread(s). The result comes
back through a lock-free/ mutex queue that `World::Update` drains — the actual
`Creature::Say` always happens on the game thread.

### Outbound HTTP client
TC ships only an *inbound* HTTP server framework (`src/common/network/Http/`). We add a
small outbound client `AISystemHttpClient` (boost::beast + already-linked OpenSSL) that:
- builds `POST https://openrouter.ai/api/v1/chat/completions`
- headers: `Authorization: Bearer <key>`, `Content-Type: application/json`,
  `HTTP-Referer` + `X-Title` (OpenRouter analytics, optional)
- body: `{ "model": "<id>", "messages": [...], "temperature": <f>, "max_tokens": <n>,
  "stream": false }`
- parses `choices[0].message.content` (JSON via TC's `nlohmann/json` or `fmt`/rapidjson).

---

## 4. Context model (the % split you asked for)

Each NPC conversation turn uses a budget of `max_tokens`/context measured in messages or
tokens. We split it:

| Slice | Config | Purpose | Now | Future |
|-------|--------|---------|-----|--------|
| System / persona | `ContextSystemRatio` (default 0.30) | NPC identity, world rules, tone, language | static persona prompt | + injected knowledge, quest state |
| History | `ContextHistoryRatio` (default 0.60) | rolling chat transcript of the exchange | kept, trimmed to fit | + long-term memory retrieval |
| Headroom | remainder | safety margin for the model reply | n/a | n/a |

History is trimmed oldest-first when it exceeds its slice (token-estimate by char count /
4). The decision model and the chat model each get their own (smaller) prompt.

---

## 5. Decision model (who responds)

Two cases from your spec:

1. **NPC says something near other NPCs** → run the decision model to pick the responder
   (unless only one other NPC is in earshot, in which case it responds directly).
2. **Player speaks** →
   - if a selected NPC exists OR an NPC is directly in front of the player → that NPC answers;
   - else the decision model picks among NPCs in earshot.
   - NPCs in earshot hear the answer and **may interject** (a second, gated call).

3. **Ambient NPC↔NPC (narrator)** → a periodic, cooldown-gated pass (every
   `NpcChatCooldownSec`, default 50s) where the decision/"narrator" model selects one NPC on
   the map to speak spontaneously. **Only runs when at least one player is present on that
   map** (`AmbientNeedsPlayer = 1`). This is the "narrator LLM selects a npc to talk" cadence.

The decision model is, by default, a **small/fast OpenRouter model** (`AISystem.ModelDecision`)
asked a structured question: "Given speakers A,B,C and the last line, who should reply? Respond
with only the GUID." A rule-based fallback (`closest other NPC`, then random) is used if the
decision call fails or is disabled — so the feature never hard-depends on a second API call.

---

## 6. Commands (this phase)

| Command | Behaviour |
|---------|-----------|
| `aisai "text"` | Force the **selected** NPC to say `text` (identical effect to `.npc say "text"`). Pure emit, no LLM. |
| `ai on` / `ai off` | Toggle ambient AI responses for the current player's session (or global via config). Lets you disable chatter. |
| (implicit) player SAY near NPC | Triggers the decision + answer flow above. |
| (implicit) NPC says near NPC | Triggers decision-model responder (ambient NPC↔NPC). |

`aisai` reuses `creature->Say(text, LANG_UNIVERSAL)` exactly like `HandleNpcSayCommand`.

---

## 7. Configuration (`worldserver.conf` / `.conf.dist`)

```
AISystem.Enabled            = 0            # off by default; set 1 to enable
AISystem.OpenRouterApiKey  = ""           # put real key in LOCAL untracked conf
AISystem.ModelChat         = "openai/gpt-4o-mini"   # overridable
AISystem.ModelDecision     = "openai/gpt-4o-mini"   # small/fast; or rule-based
AISystem.UseDecisionModel  = 1            # 0 = rule-based fallback only
AISystem.Temperature       = 0.9
AISystem.MaxTokens         = 220
AISystem.ContextSystemRatio  = 0.30
AISystem.ContextHistoryRatio = 0.60
AISystem.EarshotRadius     = 15.0         # yards; NPCs/player within this "hear"
AISystem.RequestTimeoutSec = 8
AISystem.CooldownSec       = 3            # per-NPC minimum gap between LLM replies
AISystem.InterjectChance   = 0.25         # probability an earshot NPC adds a line
AISystem.NpcChatCooldownSec= 50           # global per-map cooldown BETWEEN narrator
                                         # LLM selections of an NPC to speak (ambient)
AISystem.AmbientNeedsPlayer= 1            # 1 = NPCs only chat among themselves when a
                                         # player is present on the map; 0 = always
```

---

## 8. Module / file layout (new code, all under `src/`)

```
src/server/game/AI/LivingNPC/
    AIManager.h/.cpp          # orchestration: target selection, prompt build, enqueue
    AIService.h/.cpp          # async outbound HTTP (own io_context + worker thread)
    AIConfig.h/.cpp           # reads worldserver.conf AISystem.* keys
    AIDecision.h/.cpp         # decision model call + rule-based fallback
    AIContext.h/.cpp          # per-conversation history ring buffer + % split logic
    AIPrompt.h/.cpp           # persona/system prompt + message assembly
src/server/scripts/Commands/
    cs_ai.cpp                 # aisai / ai on|off commands
hooks:
    ChatHandler.cpp           # player SAY -> AIManager::OnPlayerSpeak
    cs_npc.cpp                # .npc say -> also notify AIManager (ambient NPC->NPC)
    World.cpp                 # World::Update drains AIService return queue
```

> `LivingNPC` is a name to avoid confusion with TC's `AI/` (creature AI scripts). Open to
> rename (e.g. `AISpeech`, `LLMNPC`).

---

## 9. Risks / open decisions

- **Latency**: even async, a 1–3 s reply feels odd in WoW. Mitigation: typewriter/emote
  before the line lands, and the cooldown prevents spam.
- **Cost**: every nearby utterance = API call. Ambient NPC↔NPC could be chatty — gated by
  `CooldownSec` + `InterjectChance` + `ai on/off`.
- **Language**: client is ptBR. Persona prompts should be locale-aware (reply in ptBR when
  the player speaks ptBR). `AISystem` can carry a `ResponseLocale` or infer from input.
- **Thread safety**: only `AIService` touches the socket; only `World::Update` touches
  `Creature`. The return queue is the single synchronization point.

---

## 10. Roadmap (future specs, not this phase)

- **Spec 2 — Prompts + Events + Dashboard**: file-driven prompt system (`AI/prompts/`,
  `AI/characters/`), configurable language (default English), event log (`ai_npc` schema,
  new `AIDatabase` pool), `ai reload`, WebUI dashboard sidecar.
- **Spec 3 — Memory**: long-term memory store + vectorization of events/chat (injects into
  the `{{memories}}` slot), retrieval injected into the system slice.
- **Spec 4 — Knowledge injection**: world/quest/lore facts pulled from DBC/DB into prompts.
- **Spec 5 — Personas**: richer per-NPC persona config (beyond the bio `.md`).
- **Spec 6 — Group/event chat**: NPCs holding a conversation among themselves on a timer.
