#!/usr/bin/env bash
# hermes-relay launcher (macOS / Linux). Run it directly, or at login via launchd/systemd.
cd "$(dirname "$0")" || exit 1
export PYTHONUTF8=1
# Optional overrides:
# export GOOGLE_API_KEY=your-google-stt-key
# export RELAY_PORT=8788
if [ -x venv-stt/bin/python ]; then
  exec ./venv-stt/bin/python relay.py >> relay.log 2>&1
else
  exec python3 relay.py >> relay.log 2>&1
fi
