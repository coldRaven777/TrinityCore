# LivingNPC Dashboard (SPEC-002 §5)

A small local web dashboard for the AI feature. It reads the **`ai_npc` MySQL schema**
(`ai_event` + `ai_character`) and reads/writes the **same `AI/` prompt + character folder**
the worldserver uses — no worldserver changes are needed for it to work.

## Run

```bat
cd C:\wow_emu\tools\ai_dashboard
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt

set AI_DB_DSN=127.0.0.1;3306;trinity;trinity;ai_npc
set AI_PROMPT_ROOT=C:\wow_emu\build\bin\RelWithDebInfo\ai   REM same root as worldserver's AISystem.PromptRoot
set AI_SERVER_CONF=C:\wow_emu\build\bin\RelWithDebInfo\worldserver.conf   REM optional: view AISystem.* keys
python app.py
```

Open http://127.0.0.1:8080

> `AI_PROMPT_ROOT` must point at the **same folder** the worldserver resolves
> `AISystem.PromptRoot` to (worldserver resolves relative paths against its own working
> directory — usually its binary folder). Edits made here are hot-reloaded by the
> worldserver on the next request, or immediately with `.ai reload`.

## Screens

| Screen | What it does |
|--------|--------------|
| **Events** | Live log (auto-refresh). Filter by type / entity uid / unprocessed. Every row with `ai_processed = 0` shows a red **NOT in AI** badge — those were logged but never fed into an LLM prompt. Row click → full JSON (related + payload). Clear button (testing only). |
| **NPCs** | List from `ai_character` (per entry). Add/register an NPC (creates the row + empty bio file). Edit bio markdown with live preview + per-spawn event stream drill-down. Toggle enabled, per-NPC model/language/temperature overrides. |
| **Prompts** | Tree of `prompts/**`. Edit any `.md`/`slots.txt`. Placeholder validation flags unknown `{{x}}`. **Assembled prompt preview** replicates the server's slot assembly (system + bio + memories + summary + events + reinforcement) for any request type. |
| **Config** | DB reachability + event count; the relevant `AISystem.*` / `AIDatabaseInfo` keys from `worldserver.conf` (if `AI_SERVER_CONF` is set). |

## Env vars

| Var | Default | Meaning |
|-----|---------|---------|
| `AI_DB_DSN` | `127.0.0.1;3306;trinity;trinity;ai_npc` | MySQL DSN (host;port;user;pass;db) |
| `AI_PROMPT_ROOT` | `ai` | Prompt/character folder root |
| `AI_SERVER_CONF` | (empty) | Optional worldserver.conf path for the Config screen |
| `AI_DASHBOARD_HOST` / `AI_DASHBOARD_PORT` | `127.0.0.1` / `8080` | Bind address |
