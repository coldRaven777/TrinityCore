# ---------------------------------------------------------------------------------------
# LivingNPC (Alive NPCs) — dashboard sidecar (SPEC-002 §5)
#
# A small FastAPI app (bind 127.0.0.1 only) that:
#   - reads the `ai_npc` schema (ai_event + ai_character) via PyMySQL
#   - reads/writes the AI/ prompt + character folders (same root worldserver uses)
#   - serves a single-page HTML dashboard
#
# The worldserver needs NO changes for this to work: it already writes events to the DB
# and hot-reloads prompt/bio files on mtime change.
#
# Run:
#   python -m venv .venv
#   .venv\Scripts\activate
#   pip install -r requirements.txt
#   set AI_DB_DSN=127.0.0.1;3306;trinity;trinity;ai_npc
#   set AI_PROMPT_ROOT=C:/wow_emu/ai          # same root as worldserver's AISystem.PromptRoot
#   python app.py                              # -> http://127.0.0.1:8080
# ---------------------------------------------------------------------------------------

import json
import os
import re
import uuid
from pathlib import Path

import pymysql
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel
from typing import Optional

# ---------------------------------------------------------------- config
DB_DSN = os.environ.get("AI_DB_DSN", "127.0.0.1;3306;trinity;trinity;ai_npc")
PROMPT_ROOT = Path(os.environ.get("AI_PROMPT_ROOT", "ai")).resolve()
SERVER_CONF = os.environ.get("AI_SERVER_CONF", "")   # optional path to worldserver.conf
HOST = os.environ.get("AI_DASHBOARD_HOST", "127.0.0.1")
PORT = int(os.environ.get("AI_DASHBOARD_PORT", "8080"))

PROMPTS_DIR = PROMPT_ROOT / "prompts"
CHARACTERS_DIR = PROMPT_ROOT / "characters"
ALLOWED_SUFFIXES = {".md", ".txt", ".md.txt"}

app = FastAPI(title="LivingNPC Dashboard", version="1.0")


def db_conn():
    parts = DB_DSN.split(";")
    host, port, user, pwd, db = parts[0], int(parts[1]), parts[2], parts[3], parts[4]
    return pymysql.connect(host=host, port=port, user=user, password=pwd,
                           database=db, charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor)


def db_error(e):
    return JSONResponse(status_code=500, content={"error": f"database error: {e}"})


# ---------------------------------------------------------------- helpers
def read_server_conf():
    """Parse AISystem.* + AIDatabaseInfo from worldserver.conf (best effort)."""
    if not SERVER_CONF or not os.path.exists(SERVER_CONF):
        return {"_note": "set AI_SERVER_CONF to the worldserver.conf path to view these"}
    out = {}
    try:
        with open(SERVER_CONF, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("["):
                    continue
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k, v = k.strip(), v.strip().strip('"').strip("'")
                if k.startswith("AISystem.") or k == "AIDatabaseInfo":
                    out[k] = v
    except Exception as e:
        out["_error"] = str(e)
    return out


def list_prompt_tree():
    tree = []
    if PROMPTS_DIR.exists():
        for p in sorted(PROMPTS_DIR.rglob("*")):
            if p.is_file() and p.suffix in ALLOWED_SUFFIXES:
                tree.append(str(p.relative_to(PROMPT_ROOT)).replace("\\", "/"))
    return tree


def safe_under(root: Path, rel: str) -> Path:
    # resolve and verify the target stays under root (path-traversal guard)
    target = (root / rel).resolve()
    if not str(target).startswith(str(root.resolve())):
        raise HTTPException(status_code=400, detail="path escapes the allowed root")
    return target


def sanitize_name(name: str) -> str:
    name = re.sub(r'[\\/:*?"<>|]', "", name)
    name = re.sub(r"\s+", " ", name).strip()
    return name


# ---------------------------------------------------------------- events
@app.get("/api/events")
def events(type: Optional[str] = None, uid: Optional[str] = None, unprocessed: Optional[int] = None,
           limit: int = 100, offset: int = 0):
    where, args = [], []
    if type:
        where.append("type = %s"); args.append(type)
    if uid:
        where.append("(actor_uid = %s OR target_uid = %s OR JSON_CONTAINS(related, %s)")
        args += [uid, uid, json.dumps(uid)]
        where[-1] += ")"
    if unprocessed is not None:
        where.append("ai_processed = %s"); args.append(int(unprocessed))
    sql = "SELECT * FROM ai_event"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY id DESC LIMIT %s OFFSET %s"
    args += [limit, offset]
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute(sql, args)
            rows = cur.fetchall()
            for r in rows:
                for k in ("related", "payload"):
                    if r.get(k) and isinstance(r[k], str):
                        try:
                            r[k] = json.loads(r[k])
                        except Exception:
                            pass
            return rows
    except Exception as e:
        return db_error(e)


@app.get("/api/events/types")
def event_types():
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT DISTINCT type FROM ai_event ORDER BY type")
            return [r["type"] for r in cur.fetchall()]
    except Exception as e:
        return db_error(e)


@app.get("/api/events/stats")
def event_stats():
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT COUNT(*) AS total, SUM(ai_processed=0) AS unprocessed, "
                        "SUM(ai_processed=1) AS processed FROM ai_event")
            row = cur.fetchone() or {}
            cur.execute("SELECT COUNT(*) AS npc FROM ai_character")
            row.update(cur.fetchone() or {})
            return row
    except Exception as e:
        return db_error(e)


@app.delete("/api/events")
def clear_events(confirm: int = 0):
    if confirm != 1:
        return JSONResponse(status_code=400, content={"error": "pass ?confirm=1"})
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("DELETE FROM ai_event")
            n = cur.rowcount
            c.commit()
            return {"deleted": n}
    except Exception as e:
        return db_error(e)


# ---------------------------------------------------------------- npcs
@app.get("/api/npcs")
def npcs():
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT * FROM ai_character ORDER BY entry")
            rows = cur.fetchall()
        for r in rows:
            bio_path = CHARACTERS_DIR / r.get("bio_file", "")
            r["bio_exists"] = bio_path.exists()
        return rows
    except Exception as e:
        return db_error(e)


@app.get("/api/npcs/{entry}")
def npc_get(entry: int):
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT * FROM ai_character WHERE entry = %s", (entry,))
            row = cur.fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="npc not found")
        bio_path = CHARACTERS_DIR / row.get("bio_file", "")
        row["bio"] = bio_path.read_text(encoding="utf-8") if bio_path.exists() else ""
        return row
    except HTTPException:
        raise
    except Exception as e:
        return db_error(e)


class NpcCreate(BaseModel):
    entry: int
    name: str


@app.post("/api/npcs")
def npc_create(body: NpcCreate):
    name = sanitize_name(body.name or "Unknown")
    bio_file = f"{body.entry} - {name}.md"
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("INSERT INTO ai_character (entry, name, bio_file) VALUES (%s, %s, %s) "
                        "ON DUPLICATE KEY UPDATE name = VALUES(name), bio_file = VALUES(bio_file)",
                        (body.entry, name, bio_file))
            c.commit()
        CHARACTERS_DIR.mkdir(parents=True, exist_ok=True)
        p = CHARACTERS_DIR / bio_file
        if not p.exists():
            p.write_text(f"# Bio — {name} (entry {body.entry})\n\n", encoding="utf-8")
        return {"ok": True, "entry": body.entry, "bio_file": bio_file}
    except Exception as e:
        return db_error(e)


class NpcUpdate(BaseModel):
    enabled: Optional[bool] = None
    model: Optional[str] = None
    temperature: Optional[float] = None
    language: Optional[str] = None
    name: Optional[str] = None


@app.put("/api/npcs/{entry}")
def npc_update(entry: int, body: NpcUpdate):
    fields, args = [], []
    for col, val in [("enabled", body.enabled), ("model", body.model),
                     ("temperature", body.temperature), ("language", body.language),
                     ("name", body.name)]:
        if val is not None:
            fields.append(f"{col} = %s"); args.append(val)
    if not fields:
        return {"ok": True}
    args.append(entry)
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute(f"UPDATE ai_character SET {', '.join(fields)} WHERE entry = %s", args)
            c.commit()
        return {"ok": True}
    except Exception as e:
        return db_error(e)


class BioWrite(BaseModel):
    content: str


@app.put("/api/npcs/{entry}/bio")
def npc_bio_write(entry: int, body: BioWrite):
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT bio_file FROM ai_character WHERE entry = %s", (entry,))
            row = cur.fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="npc not found")
    except HTTPException:
        raise
    except Exception as e:
        return db_error(e)
    bio_file = row["bio_file"] or f"{entry}.md"
    target = safe_under(CHARACTERS_DIR, bio_file)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(body.content, encoding="utf-8")
    return {"ok": True, "path": str(target.relative_to(PROMPT_ROOT))}


# ---------------------------------------------------------------- prompts
@app.get("/api/prompts")
def prompt_tree():
    return {"root": str(PROMPT_ROOT), "files": list_prompt_tree()}


@app.get("/api/prompts/file")
def prompt_file(path: str):
    target = safe_under(PROMPTS_DIR, path)
    if not target.exists():
        raise HTTPException(status_code=404, detail="file not found")
    return {"path": path, "content": target.read_text(encoding="utf-8")}


class PromptWrite(BaseModel):
    content: str


@app.put("/api/prompts/file")
def prompt_write(path: str, body: PromptWrite):
    target = safe_under(PROMPTS_DIR, path)
    if target.suffix not in ALLOWED_SUFFIXES:
        raise HTTPException(status_code=400, detail="only .md / .txt allowed")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(body.content, encoding="utf-8")
    return {"ok": True, "path": path}


# ---------------------------------------------------------------- prompt preview
BUILTIN_SYSTEM = (
    "You are {{npc}}, a creature in the world of Warcraft.\n"
    "Stay fully in character. Never mention that you are an AI or break character.\n"
    "Respond naturally and concisely (one or two sentences).\n"
    "Always reply in the language '{{language}}'."
)
BUILTIN_REINFORCEMENT = (
    "Stay in character as {{npc}}. Take into consideration that the current time is "
    "{{time}} and this conversation takes place in {{location}}. Always reply in {{language}}."
)
DEFAULT_SLOTS = [
    ("system.md", "system"), ("bio.md", "user"), ("memories.md", "user"),
    ("summary.md", "user"), ("events.md", "user"), ("reinforcement.md", "user"),
]


def load_slot(ptype: str, slot: str) -> str:
    for base in (PROMPTS_DIR / ptype, PROMPTS_DIR / "shared"):
        p = base / slot
        if p.exists():
            return p.read_text(encoding="utf-8")
    if slot == "system.md":
        return BUILTIN_SYSTEM
    if slot == "reinforcement.md":
        return BUILTIN_REINFORCEMENT
    return ""


def load_slots(ptype: str):
    manifest = PROMPTS_DIR / ptype / "slots.txt"
    slots = []
    if manifest.exists():
        for line in manifest.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            f, role = line.split(":", 1)
            slots.append((f.strip(), role.strip()))
    return slots or DEFAULT_SLOTS


def resolve(tmpl: str, ctx: dict) -> str:
    def repl(m):
        key = m.group(1)
        return ctx.get(key, m.group(0))
    return re.sub(r"\{\{\s*([a-z_]+)\s*\}\}", repl, tmpl)


class PreviewRequest(BaseModel):
    type: str = "player_chat"
    npc: str = "Sample NPC"
    entry: int = 3123
    player: str = "Player"
    target: str = ""
    language: str = "en"
    location: str = "Stormwind City (map 0) @ 1, 2"
    time: str = "12:00"
    last_line: str = ""
    bio: str = ""
    memories: str = ""
    summary: str = "Sample summary"
    events: str = "Sample events"


@app.post("/api/prompts/preview")
def prompt_preview(body: PreviewRequest):
    ctx = {
        "npc": body.npc, "npc_entry": str(body.entry), "player": body.player,
        "target": body.target, "zone": "", "map": "", "location": body.location,
        "time": body.time, "language": body.language, "last_line": body.last_line,
        "bio": body.bio, "memories": body.memories, "summary": body.summary,
        "events": body.events,
    }
    system, user = "", ""
    for slot, role in load_slots(body.type):
        resolved = resolve(load_slot(body.type, slot), ctx).strip()
        if not resolved:
            continue
        if role == "system":
            system += ("\n\n" if system else "") + resolved
        else:
            user += ("\n\n" if user else "") + resolved
    if not system:
        system = resolve(BUILTIN_SYSTEM, ctx)
    return {"type": body.type, "system": system, "user": user,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}]}


# ---------------------------------------------------------------- misc
@app.get("/api/config")
def config():
    return {"prompt_root": str(PROMPT_ROOT),
            "db_dsn": DB_DSN,
            "server_conf": read_server_conf()}


@app.get("/api/status")
def status():
    out = {"db": False, "db_error": None, "prompt_root": str(PROMPT_ROOT),
           "prompt_root_exists": PROMPT_ROOT.exists()}
    try:
        with db_conn() as c, c.cursor() as cur:
            cur.execute("SELECT COUNT(*) AS n FROM ai_event")
            out["db"] = True
            out["event_count"] = cur.fetchone()["n"]
    except Exception as e:
        out["db_error"] = str(e)
    return out


@app.get("/")
def index():
    return FileResponse(os.path.join(os.path.dirname(__file__), "static", "index.html"))


if __name__ == "__main__":
    import uvicorn
    print(f"LivingNPC dashboard -> http://{HOST}:{PORT}  (prompt root: {PROMPT_ROOT})")
    uvicorn.run(app, host=HOST, port=PORT)
