@echo off
REM hermes-relay launcher (Windows). Keep running; or set it to start at logon
REM (Task Scheduler: "Run at log on" -> this file).
cd /d "%~dp0"
set PYTHONUTF8=1
REM Optional overrides:
REM set GOOGLE_API_KEY=your-google-stt-key
REM set RELAY_PORT=8788
if exist venv-stt\Scripts\python.exe (
  venv-stt\Scripts\python.exe relay.py >> relay.log 2>&1
) else (
  python relay.py >> relay.log 2>&1
)
