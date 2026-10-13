# hermes-relay — the reference agent bridge

The bridge the gadget talks to when it is *not* talking to Muse directly. One process,
Python 3 standard library only, HTTP on `:8788` behind a shared token. It is the server
half of [`../docs/AGENT-PROTOCOL.md`](../docs/AGENT-PROTOCOL.md): the firmware speaks
that contract to whatever URL you configure — this is a working implementation to start
from, not a requirement.

```
gadget ──voice note / text──▶ relay.py ──STT──▶ your agent endpoint ──▶ reply text
       ◀──streamed reply + TTS (MP3)──                     (Hermes API server, or any
       ◀──pushed notifications── notifications.py / notify.py    OpenAI-compatible /v1)
```

Not tied to Hermes: the relay POSTs to an OpenAI-compatible `/v1/chat/completions`;
point `HERMES_URL` at any endpoint of that shape (a local LLM server, another agent
framework) or edit the single call site in `relay.py`.

## What's here

| File | Role |
|---|---|
| `relay.py` | The HTTP server: `POST /turn` (voice note or `?text=`, `?stream=1` for NDJSON streaming), `/notifications*`, `/agents`. The full contract is documented in `../docs/AGENT-PROTOCOL.md`. |
| `agents.py`, `agents.json` | Agent registry: which backing profile/endpoint each gadget agent slot uses, its label and voice. `agents.json` is the file you edit; unknown names fall back to `default` with a warning, never an error. |
| `notifications.py`, `notify.py` | The reverse channel: a store (`add/list/pending/history/ack/cancel`) plus a CLI to raise a nudge or a reminder (`notify.py <agent> "text" --in 8m`). |
| `new_agent.py` | Validates and adds a new agent entry to `agents.json`. |
| `whisper_worker.py` | Optional local speech-to-text (faster-whisper) — used automatically when no Google STT key is set. |
| `relay-launch.cmd` / `relay-launch.sh` | Run scripts for Windows / macOS-Linux. |

## Quick start — Windows

1. Install Python 3.11+ (python.org; check "Add to PATH").
2. Optional local STT: `python -m venv venv-stt && venv-stt\Scripts\pip install faster-whisper`
3. Optional cloud STT (faster, ~0.6 s): set `GOOGLE_API_KEY` in the shell, or save your
   key to `google.key` next to the files. Without either, turns use the local model.
4. Edit `agents.json` — at minimum the `default` entry. Each entry wants `label`,
   `voice`, an `api_key_file` (a file containing your agent endpoint's bearer key) and
   optionally `voice_name` (a Google TTS voice) and `personality_file`.
5. Run `relay-launch.cmd`. First run creates the shared `token` file.
6. On the gadget side: set `CONFIG_MUSE_HERMES_URL="http://<this-pc-ip>:8788"` and
   `CONFIG_MUSE_HERMES_TOKEN="<token contents>"` in your board profile, rebuild, flash.
   The serial console can also seed the address live: `>relay.lan=<this-pc-ip>`.

## Quick start — macOS / Linux

Same as above, plus: `python3 -m venv venv-stt && venv-stt/bin/pip install faster-whisper`
and run `./relay-launch.sh`. Nothing else differs — it is stdlib Python.

## The token

The relay writes `token` on first run. The gadget must send it as `X-Gadget-Token` on
every request; any mismatch is a 401. Keep the file out of git (the bundled `.gitignore`
does) and use HTTPS (a Tailscale funnel, or any TLS reverse proxy) if you expose the
relay beyond your own network.

## Tests

```bash
python -m unittest test_relay test_notifications     # ~76 tests, no network needed
```

## Security posture

- Every endpoint requires the shared token (constant-time compare).
- Secrets stay server-side: agent bearer keys are read from files you point at;
  the Google key from the environment or a file. Nothing is logged or echoed.
- This is a LAN/personal-scale bridge. Exposing it publicly is your call: put TLS in
  front and keep the token long.
