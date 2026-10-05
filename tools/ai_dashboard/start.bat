@echo off
rem LivingNPC dashboard launcher (SPEC-002 §5). Point these at your local setup.
set AI_DB_DSN=127.0.0.1;3306;trinity;trinity;ai_npc
set AI_PROMPT_ROOT=C:/wow_emu/build/bin/RelWithDebInfo/ai
set AI_SERVER_CONF=C:/wow_emu/build/bin/RelWithDebInfo/worldserver.conf
cd /d "%~dp0"
".\.venv\Scripts\python.exe" app.py