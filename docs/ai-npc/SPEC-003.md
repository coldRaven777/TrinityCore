# Spec 003 — NPC "self-awareness" (vitals), proximity-correct responder selection, and ambient throttle

**Depends on:** PROJECT.md §2–§8, SPEC-001 (implemented), SPEC-002 (implemented, IMPLEMENTATION.md as-built).
**Branch:** `feature/ai-npc`

> **Roadmap renumber note:** SPEC-002 reserved the name "Spec 003" for *memory + vectorization*.
> This spec **reassigns Spec 003** to the three features below (user request, 2026-10-05).
> Memory + vectorization slides to a later spec number (tentatively **Spec 005**); this doc
> does not implement memory. `{{memories}}` stays empty until that later spec.

---

## 0. Scope (this spec only)

1. **NPC vitals injection** — every assembled prompt gains a `[current vitals]` block
   describing the speaking NPC's equipment, faction, race/type, gender, and sub-title.
2. **Proximity- and context-correct responder selection** — fix the ".npc say → wrong NPC
   answers" bug by ranking candidate responders on *distance to the speaker* first, then
   context, then (optionally) the decision model.
3. **Ambient throttle** — NPC-initiated speech (the narrator timer) is slowed down and made
   cheaper: default cadence raised to ~60 s, with a hard global cap and smarter gating so
   NPCs stop over-talking.

**Out of scope (later):** memory/vectorization (Spec 005), knowledge injection (Spec 004
per SPEC-002), quest hooks, timed group chat.

---

## 1. NPC vitals — "know thyself"

### 1.1 Goal

An NPC should be able to ground its reply in *what it physically is and holds*. This is the
`[current equipment]` / faction / race / gender / subname block the user described. It is
injected as a **new prompt slot** so it can be tuned or suppressed per request type, and so
it shows in the dashboard's assembled-prompt preview.

### 1.2 Data sources (all available today in `feature/ai-npc`)

| Vitals field | Source in engine | Notes |
|---|---|---|
| Equipment | `sObjectMgr->GetEquipmentInfo(entry, id)` → `EquipmentInfo::Items[MAX_EQUIPMENT_ITEMS]` (each `ItemId`) → `sObjectMgr->GetItemTemplate(ItemId)->GetName(locale)` | Up to 3 slots (main/secondary/ranged). `ItemId == 0` ⇒ empty hand. |
| Faction | `creature_template.faction` → `sFactionTemplateStore.LookupEntry(faction)` → `sFactionStore.LookupEntry(template.Faction)` → `FactionEntry::Name` | Two-step lookup. If any step is missing, emit the numeric id with no name rather than crashing. |
| Race / kind | `creature_template.family` (enum `CreatureFamily`) + `creature_template.type` (enum `CreatureType`) | **Creatures have NO player "race" field.** The closest obtainable analog is **family** (e.g. "Wolf", "Humanoid") and **type**. We label the block `race/family` and fill it from `CreatureFamily`/`CreatureType`; for non-beast types we fall back to the type string (e.g. "Humanoid", "Mechanical"). This is the honest mapping — there is no `race` column on creatures. |
| Gender | `Unit::GetGender()` → `Gender` enum → `GENDER_MALE`/"male", `GENDER_FEMALE`/"female", `GENDER_NONE`/"none" (or "unknown" if `GENDER_UNKNOWN`). | Directly available. |
| SubName (title) | `creature_template.SubName` (the line under the name, e.g. "<The Defias>") | **Per user decision (2026-10-05): this replaces the originally-requested "guild". NPCs have no guild; the sub-title is the closest "who they belong to" field.** If empty, emit nothing for that line. |

> **Guild decision (resolved):** the user explicitly chose to **drop "guild" entirely** and
> inject only `SubName` (title). The vitals block therefore has no `guild` line.

### 1.3 New slot

Add a new slot file `vitals.md` to every prompt type (`player_chat`, `npc_chat`, `ambient`)
and to `shared/`. It is rendered from a **new placeholder set** and assembled in
`AIPrompt::Build` like any other slot. Default content (shared fallback):

```markdown
[current vitals]
- race/family: {{race}}
- gender: {{gender}}
- faction: {{faction}}
- subname: {{subname}}
[current equipment]
{{equipment}}
```

Rendered example (armed NPC):

```markdown
[current vitals]
- race/family: Humanoid
- gender: male
- faction: Defias Brotherhood
- subname: <The Defias>
[current equipment]
- Sword of Talos
- (empty hand)
- (empty hand)
```

Rendered example (nothing equipped, no subname):

```markdown
[current vitals]
- race/family: Mechanical
- gender: none
- faction: Gnomeregan Exiles
- subname:
[current equipment]
- none
```

### 1.4 Placeholder additions (resolve in `AIPrompt::Resolve`)

| Placeholder | Resolved to |
|---|---|
| `{{race}}` | creature family string (or creature type string if family is `CREATURE_FAMILY_NONE`) |
| `{{gender}}` | "male" / "female" / "none" / "unknown" |
| `{{faction}}` | faction display name (or `"<id>"` numeric if lookup fails) |
| `{{subname}}` | `creature_template.SubName` (may be empty) |
| `{{equipment}}` | `- none` if no items, else `- <item name>` per equipped slot |

The vitals string is computed once in `AIManager::RequestReply` / `UpdateAmbient` (a small
helper `BuildVitals(Creature*)`), then passed into `AIPromptInput` as a new field
`vitals` so the resolver can drop it into `{{...}}` without re-deriving per slot. (Keeps
`AIPrompt` free of game-type lookups — it stays a pure text assembler, per SPEC-002 §2.1.)

### 1.5 Locale

Item / faction names resolve through `DEFAULT_LOCALE` (server locale). Future per-player
locale (Spec 004) is out of scope; we do **not** attempt to localize to the speaking
client now.

---

## 2. Responder selection — fix ".npc say → wrong NPC answers"

### 2.1 The bug

`AIManager::OnNpcSpeak` (wired from `.npc say` in `cs_npc.cpp:949`) collects *all* creatures
within `EarshotRadius` of the speaker, then:

- if exactly 1 other → that one answers;
- else → calls `AIDecision::Decide(...)` (the decision/LLM model), **or** falls back to
  `others.front()` (the first creature the map store happened to yield — which is
  *unordered* and frequently a far-away / out-of-frame NPC like "NPC Y").

The decision model is given only a flat name list with **no positional context**, so it
often picks an NPC that is not adjacent to the speaker. The rule fallback (`others.front()`)
is even worse — it is essentially random w.r.t. distance.

### 2.2 Fix — distance-first ranking, context second, model last

Replace the selection logic in `OnNpcSpeak` (and symmetrically tighten `OnPlayerSpeak`'s
front-vector fallback) with a **distance-ranked candidate list**:

1. **Collect + distance-rank.** `CollectNearby(speaker, radius)` already exists; sort the
   resulting `others` by `speaker->GetDistance(c)` ascending. The closest *other* creature
   to the speaker is candidate #1.
2. **Context tie-break (cheap, no LLM).** Before calling the decision model, apply a
   positional heuristic:
   - Prefer a candidate that is **directly between / facing** the speaker relative to where
     the line is "aimed" (reuse the existing front-vector idea from `OnPlayerSpeak`, but
     measured from the *speaker's* orientation, not the player's).
   - Prefer a candidate whose distance is within, say, `min(EarshotRadius, 8.0)` yards of the
     speaker — i.e. genuinely "right next to NPC A" — over a farther one.
3. **Decision model (optional, when `UseDecisionModel` + valid key).** Call `AIDecision` with
   the **distance-ranked, distance-annotated** candidate list (e.g. `"GUID=… Name=… dist=3.2yd"`)
   and an explicit instruction: *"The speaker is at the origin; prefer the closest NPC that
   fits the context. Reply with only the chosen GUID."* The model may still override, but it
   now sees distances, so "NPC Y out of frame" is unlikely to win.
4. **Rule fallback (deterministic, distance-correct).** If the model is off / no key / returns
   nothing valid → **pick `others[0]` of the *sorted* list** (the closest one), NOT the raw
   map-store front. This alone fixes the most common failure (the wrong, far NPC answering).

Add config `AISystem.ResponderDistanceBias` (default `1.0`, float) — a weight that, when > 0,
makes the rule fallback strictly prefer the closest; set 0 to restore legacy "first in store"
behavior. (Kept so the change is reversible / tunable.)

### 2.3 `AIDecision` prompt update

`AIDecision::Decide` currently builds a name-only list. Change the `list` builder to include
distance (passed in) and add to the system prompt: *"Candidates are listed with their
distance in yards from the speaking NPC. Strongly prefer the nearest candidate that makes
sense in context; do not pick a distant NPC just because its name appears early."* Keep the
existing "reply with only the GUID" contract and the `RuleFallback` (now distance-sorted).

### 2.4 Where this applies

- `OnNpcSpeak` (`.npc say`, NPC→NPC) — **primary fix site**.
- `OnPlayerSpeak` player→NPC — already uses selected/front-vector; add the same
  distance-sorted fallback so a far mob never steals the player's line either.
- `MaybeInterject` — already picks the first `nearby != speaker`; change to the closest
  `!= speaker` for consistency.

---

## 3. Ambient throttle — NPCs talk less

### 3.1 Problem

`UpdateAmbient` fires a **random** creature's narrator prompt every `NpcChatCooldownSec`
(default **50 s**) *per map*, gated only by `AmbientNeedsPlayer`. With several creatures in
`ActivationRadius` this still produces a lot of token-burning LLM calls, and the user reports
NPCs "talking too much".

### 3.2 Changes

1. **Raise default cadence to 60 s.** Set `AIConfig::m_npcChatCooldown` default `50` → `60`.
   (Config key `AISystem.NpcChatCooldownSec`, already exists.)
2. **Hard global cap + per-map guard.** Keep the per-map timer, but additionally enforce:
   - A **global** minimum gap between *any* ambient narrator calls across all maps
     (`AISystem.AmbientGlobalCooldownSec`, default e.g. `30`) so even on a multi-map server
     we don't stack narrator calls back-to-back. (Track last ambient fire time in
     `AIManager`; ambient is already single-threaded on `World::Update`, so a plain
     `uint64` guarded by the existing update flow is sufficient — no new mutex needed.)
   - Skip ambient entirely if the player has **no nearby speakable NPC** (already partially
     done; make the empty-check a hard bail, not just an empty pick).
3. **Reduce wasted calls — "is there anything to say?":** before spending an LLM token,
   cheaply check that the chosen narrator has *something* in `{{events}}` / `{{summary}}` /
   a non-empty bio, OR is near the player (already true). If the chosen creature's recent
   context is empty AND its bio is empty, **skip this tick** (don't call the model) and reset
   the timer. This cuts silent "thinking aloud" filler that the user finds noisy.
4. **Respect session disable.** `AmbientNeedsPlayer` already gates on *any* active session;
   additionally honor per-session `ai off` (currently ambient ignores session toggle — make
   it bail if the *ref* player's session has AI disabled). Minor but correct.
5. **(Optional, config-off by default) burst cap:** `AISystem.AmbientMaxPerMinute` (default
   `2`) — if more than N ambient fires would occur in a rolling minute, throttle. Off by
   default (set 0) to avoid surprising behavior; documented for tuning.

> The user's phrasing "every 60 seconds or something" is met by #1 (default 60 s) plus #2/#3
> which make those 60 s actually *mean* something (no filler, no stacking).

### 3.3 Config additions / changes

```ini
# --- LivingNPC / Spec 003 ---
AISystem.NpcChatCooldownSec   = 60     # was 50; global per-map narrator cadence (seconds)
AISystem.AmbientGlobalCooldownSec = 30 # min gap between ANY ambient narrator call (all maps)
AISystem.ResponderDistanceBias  = 1.0  # distance weighting for responder rule-fallback (0 = legacy)
AISystem.AmbientMaxPerMinute     = 0   # 0 = unlimited; else hard burst cap (tuning knob)
```

`EarshotRadius`, `ActivationRadius`, `CooldownSec`, `InterjectChance`, `AmbientNeedsPlayer`
are unchanged (tunable as before).

---

## 4. Acceptance / smoke test

### Vitals
1. Boot with an armed Defias NPC near a player; `.npc say "who are you?"` → the responder's
   assembled prompt (dashboard preview, or add a temporary `TC_LOG_DEBUG` of the system
   string) shows `[current vitals]` with faction "Defias Brotherhood", gender, and
   `[current equipment]` listing the actual item names from `creature_equip_template`.
2. Unarmed NPC → `[current equipment]` shows `- none`. Missing faction lookup → faction line
   shows the numeric id, no crash.
3. SubName (title) appears when set; absent when empty. No "guild" line anywhere (per decision).

### Responder selection
4. Place NPC A and NPC B adjacent (≤ a few yards), NPC Y ~30 yards away (still within
   earshot). `.npc say` from A → **B** answers, never Y. Repeat 10×; Y must never win.
5. With decision model on, the candidate list passed to `AIDecision` includes distances; the
   model still picks B (closest, in-frame).
6. Decision model off → rule fallback picks the closest other (B), deterministically.

### Ambient throttle
7. With one player + a few NPCs in range, ambient narrator fires roughly every 60 s (not 50),
   and never stacks (global cooldown). Count LLM ambient calls over 5 min ≈ 5 (was ~6, and
   with #3 skip, often fewer when context is empty).
8. Ambient does NOT fire when the ref player has `ai off` (session disable honored).
9. No crash / no freeze when `ActivationRadius` excludes all NPCs (hard bail, no empty pick).

---

## 5. Files touched

- **Edit:** `AIPrompt.{h,cpp}` — new vitals placeholders + resolver cases; `AIPromptInput`
  gains `vitals` field; seed `vitals.md` into `shared/` + each type's default tree; add
  `vitals.md` to the default `slots.txt` order (after `bio.md`, before `events.md`).
- **Edit:** `AIConfig.{h,cpp}` — `m_npcChatCooldown` default 50→60; add
  `AmbientGlobalCooldownSec`, `ResponderDistanceBias`, `AmbientMaxPerMinute` (+ accessors);
  `worldserver.conf.dist` + local `worldserver.conf` (untracked) get the new keys.
- **Edit:** `AIManager.{h,cpp}` — `BuildVitals(Creature*)` helper; distance-sort candidates in
  `OnNpcSpeak`/`OnPlayerSpeak`/`MaybeInterject`; global ambient cooldown + empty-context skip
  + session-disable honor in `UpdateAmbient`.
- **Edit:** `AIDecision.cpp` — distance-annotated candidate list + "prefer nearest in context"
  instruction in the decision system prompt.
- **Dashboard:** `tools/ai_dashboard/` — assembled-prompt preview already shows slots; vitals
  will appear automatically once `vitals.md` is a slot. No schema change. (Optionally surface
  the new config keys in the Config screen.)

---

## 6. Decisions (confirmed 2026-10-05)

1. **Guild → SubName only.** NPCs have no guild; inject `SubName` (title) instead. No guild
   line in vitals. (User decision.)
2. **Race = family/type.** Creatures have no player `race`; closest obtainable is
   `CreatureFamily`/`CreatureType`. Labeled `race/family` honestly.
3. **Responder = distance-first.** Sort candidates by distance to the *speaker*; model is a
   tie-breaker with distances shown; rule fallback = closest (not map-store front).
4. **Ambient default 60 s** + global cooldown + empty-context skip + session-disable honor.
5. **No new DB tables, no memory.** This spec is prompt-injection + selection + throttling
   only; `{{memories}}` stays empty (memory is Spec 005 now).

---

## 7. Implementation status

**Implemented on `feature/ai-npc` and compiled (2026-10-05).** `game.lib` + `worldserver.exe`
rebuilt with **0 errors** (`cmake --build . --target worldserver`, RelWithDebInfo). Changes:

- `AIConfig.{h,cpp}` — `NpcChatCooldownSec` default 50→60; new `AmbientGlobalCooldownSec`
  (30), `ResponderDistanceBias` (1.0), `AmbientMaxPerMinute` (0) + accessors + config reads.
- `AIPrompt.{h,cpp}` — vitals placeholders (`race`/`gender`/`faction`/`subname`/`equipment`)
  resolved in `AIPrompt::Resolve`; seeded `vitals.md` (shared + per-type) and added to default
  `slots.txt` order (after `bio.md`).
- `AIManager.{h,cpp}` — `BuildVitals(Creature*)` (faction family/type, gender, faction name
  via FactionTemplate→Faction, SubName, equipment via `GetEquipmentInfo`+`GetItemTemplate`);
  `SortByDistance` helper; distance-sorted responder selection in `OnNpcSpeak`/`OnPlayerSpeak`
  (rule fallback = closest, not map-store front) and `MaybeInterject`; ambient throttle:
  global cooldown, rolling-minute burst cap, per-session `ai off` honor, and empty-context
  skip (no bio + no summary + no events → don't call the model).
- `AIDecision.cpp` — system prompt now lists candidate distances and instructs "prefer the
  closest in context".
- `worldserver.conf.dist` — `NpcChatCooldownSec = 60` + three new Spec 003 keys documented.

**Not yet smoke-tested in-game** (needs a running server + player + NPCs). The code paths
compile and the logic is wired; runtime verification (vitals appear in prompt, wrong-NPC
bug fixed, ambient cadence) is pending a live session. Update acceptance items 1–9 once run.

**Known caveats:** `{{memories}}` still empty (memory is now Spec 005); `ResponderDistanceBias`
is currently a documented hook (selection is already distance-first via `SortByDistance`);
local `worldserver.conf` (untracked) should add the three new keys if it overrides them.
